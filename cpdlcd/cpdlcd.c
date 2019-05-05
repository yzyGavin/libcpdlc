/*
 * Copyright 2019 Saso Kiselkov
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <curl/curl.h>
#include <gnutls/gnutls.h>

#include <acfutils/avl.h>
#include <acfutils/conf.h>
#include <acfutils/crc64.h>
#include <acfutils/htbl.h>
#include <acfutils/log.h>
#include <acfutils/list.h>
#include <acfutils/safe_alloc.h>

#include "../src/cpdlc.h"
#include "blocklist.h"
#include "common.h"

#define	CALLSIGN_LEN		16
#define	CONN_BACKLOG		UINT16_MAX
#define	READ_BUF_SZ		4096
#define	MAX_BUF_SZ		8192
#define	MAX_BUF_SZ_NO_LOGON	128
#define	POLL_TIMEOUT		1000	/* ms */
#define	QUEUED_MSG_TIMEOUT	3600	/* seconds */

typedef struct {
	char		callsign[CALLSIGN_LEN];
	avl_node_t	node;
} atc_t;

typedef struct {
	char		from[CALLSIGN_LEN];
	char		to[CALLSIGN_LEN];
	bool		logon_complete;

	uint8_t		addr[MAX_ADDR_LEN];
	socklen_t	addr_len;
	int		addr_family;
	int		fd;

	gnutls_session_t	session;
	bool			tls_handshake_complete;

	uint8_t		*inbuf;
	size_t		inbuf_sz;
	uint8_t		*outbuf;
	size_t		outbuf_sz;

	avl_node_t	node;
	avl_node_t	from_node;
} conn_t;

typedef struct {
	char		from[CALLSIGN_LEN];
	char		to[CALLSIGN_LEN];
	time_t		created;
	char		*msg;
	list_node_t	node;
} queued_msg_t;

typedef struct {
	uint8_t		addr[MAX_ADDR_LEN];
	socklen_t	addr_len;
	int		addr_family;
	int		fd;
	avl_node_t	node;
} listen_sock_t;

static avl_tree_t	atcs;
static avl_tree_t	conns;
static htbl_t		conns_by_from;
static avl_tree_t	listen_socks;
static char		keyfile[PATH_MAX] = { 0 };
static char		certfile[PATH_MAX] = { 0 };
static char		cafile[PATH_MAX] = { 0 };

static list_t		queued_msgs;
static uint64_t		queued_msg_bytes = 0;
static uint64_t		queued_msg_max_bytes = 128 << 20;	/* 128 MiB */

static gnutls_certificate_credentials_t	x509_creds;
static gnutls_priority_t		prio_cache;

static bool		background = true;
static bool		do_shutdown = false;
static int		default_port = 17622;

static void send_error_msg(conn_t *conn, const cpdlc_msg_t *orig_msg,
    const char *fmt, ...);
static void close_conn(conn_t *conn);

static bool
set_sock_nonblock(int fd)
{
	int flags;

	return ((flags = fcntl(fd, F_GETFL)) >= 0 &&
	    fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0);
}

static int
atc_compar(const void *a, const void *b)
{
	const atc_t *aa = a, *ab = b;
	int res = strcmp(aa->callsign, ab->callsign);
	if (res < 0)
		return (-1);
	if (res > 0)
		return (1);
	return (0);
}

static int
conn_compar(const void *a, const void *b)
{
	const conn_t *ca = a, *cb = b;
	int res;

	if (ca->addr_len < cb->addr_len)
		return (-1);
	if (ca->addr_len > cb->addr_len)
		return (1);
	res = memcmp(ca->addr, cb->addr, sizeof (ca->addr_len));
	if (res < 0)
		return (-1);
	if (res > 0)
		return (1);
	return (0);
}

static int
listen_sock_compar(const void *a, const void *b)
{
	const listen_sock_t *la = a, *lb = b;
	int res;

	if (la->addr_len < lb->addr_len)
		return (-1);
	if (la->addr_len > lb->addr_len)
		return (1);
	res = memcmp(la->addr, lb->addr, sizeof (la->addr_len));
	if (res < 0)
		return (-1);
	if (res > 0)
		return (1);
	return (0);
}

static void
init_structs(void)
{
	avl_create(&atcs, atc_compar, sizeof (atc_t), offsetof(atc_t, node));
	avl_create(&conns, conn_compar, sizeof (conn_t),
	    offsetof(conn_t, node));
	htbl_create(&conns_by_from, 1024, CALLSIGN_LEN, true);
	list_create(&queued_msgs, sizeof (queued_msg_t),
	    offsetof(queued_msg_t, node));
	avl_create(&listen_socks, listen_sock_compar, sizeof (listen_sock_t),
	    offsetof(listen_sock_t, node));
	blocklist_init();
}

static void
fini_structs(void)
{
	void *cookie;
	atc_t *atc;
	conn_t *conn;
	queued_msg_t *msg;
	listen_sock_t *ls;

	cookie = NULL;
	while ((atc = avl_destroy_nodes(&atcs, &cookie)) != NULL)
		free(atc);
	avl_destroy(&atcs);

	htbl_empty(&conns_by_from, NULL, NULL);
	htbl_destroy(&conns_by_from);

	cookie = NULL;
	while ((conn = avl_destroy_nodes(&conns, &cookie)) != NULL) {
		close(conn->fd);
		free(conn);
	}
	avl_destroy(&conns);

	while ((msg = list_remove_head(&queued_msgs)) != NULL) {
		free(msg->msg);
		free(msg);
	}
	list_destroy(&queued_msgs);
	queued_msg_bytes = 0;

	cookie = NULL;
	while ((ls = avl_destroy_nodes(&listen_socks, &cookie)) != NULL) {
		if (ls->fd != -1)
			close(ls->fd);
		free(ls);
	}
	avl_destroy(&listen_socks);

	blocklist_fini();
}

static void
print_usage(const char *progname, FILE *fp)
{
	fprintf(fp, "Usage: %s [-h] [-c <conffile>]\n", progname);
}

static bool
add_atc(const char *callsign)
{
	atc_t *atc = safe_calloc(1, sizeof (*atc));
	atc_t *old_atc;
	avl_index_t where;

	lacf_strlcpy(atc->callsign, callsign, sizeof (atc->callsign));
	old_atc = avl_find(&atcs, atc, &where);
	if (old_atc != NULL) {
		fprintf(stderr, "Duplicate ATC entry %s", callsign);
		return (false);
	}
	avl_insert(&atcs, atc, where);

	return (true);
}

static bool
add_listen_sock(const char *name_port)
{
	char hostname[64];
	int port = default_port;
	const char *colon = strchr(name_port, ':');
	struct addrinfo *ai_full = NULL;
	char portbuf[8];
	int error;

	if (colon != NULL) {
		lacf_strlcpy(hostname, name_port, (colon - name_port) + 1);
		if (sscanf(&colon[1], "%d", &port) != 1 ||
		    port <= 0 || port > UINT16_MAX) {
			fprintf(stderr, "Invalid listen directive \"%s\": "
			    "expected valid port number after ':' character\n",
			    name_port);
			return (false);
		}
	} else {
		lacf_strlcpy(hostname, name_port, sizeof (hostname));
	}
	snprintf(portbuf, sizeof (portbuf), "%d", port);

	error = getaddrinfo(hostname, portbuf, NULL, &ai_full);
	if (error != 0) {
		fprintf(stderr, "Invalid listen directive \"%s\": %s\n",
		    name_port, gai_strerror(error));
		return (false);
	}

	for (const struct addrinfo *ai = ai_full; ai != NULL;
	    ai = ai->ai_next) {
		listen_sock_t *ls, *old_ls;
		avl_index_t where;
		unsigned int one = 1;

		if (ai->ai_protocol != IPPROTO_TCP)
			continue;

		ls = safe_calloc(1, sizeof (*ls));
		ASSERT3U(ai->ai_addrlen, <=, sizeof (ls->addr));
		memcpy(ls->addr, ai->ai_addr, ai->ai_addrlen);
		ls->addr_len = ai->ai_addrlen;
		ls->addr_family = ai->ai_family;

		old_ls = avl_find(&listen_socks, ls, &where);
		if (old_ls != NULL) {
			fprintf(stderr, "Invalid listen directive \"%s\": "
			    "address already used on another socket\n",
			    name_port);
			free(ls);
			goto errout;
		}
		avl_insert(&listen_socks, ls, where);

		ls->fd = socket(ai->ai_family, ai->ai_socktype,
		    ai->ai_protocol);
		if (ls->fd == -1) {
			fprintf(stderr, "Invalid listen directive \"%s\": "
			    "cannot create socket: %s\n", name_port,
			    strerror(errno));
			goto errout;
		}
		setsockopt(ls->fd, SOL_SOCKET, SO_REUSEADDR, &one,
		    sizeof (one));
		if (bind(ls->fd, ai->ai_addr, ai->ai_addrlen) == -1) {
			fprintf(stderr, "Invalid listen directive \"%s\": "
			    "cannot bind socket: %s\n", name_port,
			    strerror(errno));
			goto errout;
		}
		if (listen(ls->fd, CONN_BACKLOG) == -1) {
			fprintf(stderr, "Invalid listen directive \"%s\": "
			    "cannot listen on socket: %s\n", name_port,
			    strerror(errno));
			goto errout;
		}
		if (!set_sock_nonblock(ls->fd)) {
			fprintf(stderr, "Invalid listen directive \"%s\": "
			    "cannot set socket as non-blocking: %s\n",
			    name_port, strerror(errno));
			goto errout;
		}
	}

	freeaddrinfo(ai_full);
	return (true);
errout:
	if (ai_full != NULL)
		freeaddrinfo(ai_full);
	return (false);
}

static bool
parse_config(const char *conf_path)
{
	int errline;
	conf_t *conf = conf_read_file(conf_path, &errline);
	const char *key, *value;
	void *cookie;

	if (conf == NULL) {
		if (errline == -1) {
			fprintf(stderr, "Can't open %s: %s\n", conf_path,
			    strerror(errno));
		} else {
			fprintf(stderr, "%s: parsing error on %d\n",
			    conf_path, errline);
		}
		return (false);
	}

	cookie = NULL;
	while (conf_walk(conf, &key, &value, &cookie)) {
		if (strncmp(key, "atc/name/", 9) == 0) {
			if (!add_atc(value))
				goto errout;
		} else if (strncmp(key, "listen/", 7) == 0) {
			if (!add_listen_sock(value))
				goto errout;
		} else if (strcmp(key, "keyfile") == 0) {
			lacf_strlcpy(keyfile, value, sizeof (keyfile));
		} else if (strcmp(key, "certfile") == 0) {
			lacf_strlcpy(certfile, value, sizeof (certfile));
		} else if (strcmp(key, "cafile") == 0) {
			lacf_strlcpy(cafile, value, sizeof (cafile));
		} else if (strcmp(key, "blocklist") == 0) {
			blocklist_set_filename(value);
		}
	}

	if (avl_numnodes(&atcs) == 0 && !add_atc("TEST"))
		goto errout;
	if (avl_numnodes(&listen_socks) == 0 && !add_listen_sock("localhost"))
		goto errout;

	conf_free(conf);
	return (true);
errout:
	conf_free(conf);
	return (false);
}

static bool
auto_config(void)
{
	return (add_listen_sock("localhost") && add_atc("TEST"));
}

static bool
daemonize(bool do_chdir, bool do_close)
{
	switch (fork()) {
	case -1:
		perror("fork error");
		return (false);
	case 0:
		break;
	default:
		_exit(EXIT_SUCCESS);
	}
	if (setsid() < 0) {
		perror("setsid error");
		return (false);
	}
	if (do_chdir)
		chdir("/");
	if (do_close) {
		close(STDIN_FILENO);
		if (open("/dev/null", O_RDONLY) < 0) {
			perror("Cannot replace STDIN with /dev/null");
			return (false);
		}
	}

	return (true);
}

static void
handle_accepts(listen_sock_t *ls)
{
	for (;;) {
		conn_t *conn = safe_calloc(1, sizeof (*conn));
		conn_t *old_conn;
		avl_index_t where;

		conn->addr_len = sizeof (conn->addr);
		conn->fd = accept(ls->fd, (struct sockaddr *)conn->addr,
		    &conn->addr_len);
		conn->addr_family = ls->addr_family;
		if (conn->fd == -1) {
			free(conn);
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				fprintf(stderr, "Error accepting connection: "
				    "%s", strerror(errno));
				continue;
			} else {
				break;
			}
		}
		if (!blocklist_check(conn->addr, conn->addr_len,
		    conn->addr_family)) {
			fprintf(stderr, "Incoming connection blocked: "
			    "address on blocklist.\n");
			close(conn->fd);
			free(conn);
			continue;
		}

		set_sock_nonblock(conn->fd);
		old_conn = avl_find(&conns, conn, &where);
		if (old_conn != NULL) {
			fprintf(stderr, "Error accepting connection: "
			    "duplicate connection encountered?!");
			close(conn->fd);
			free(conn);
			continue;
		}

		VERIFY0(gnutls_init(&conn->session,
		    GNUTLS_SERVER | GNUTLS_NONBLOCK | GNUTLS_NO_SIGNAL));
		VERIFY0(gnutls_priority_set(conn->session, prio_cache));
		VERIFY0(gnutls_credentials_set(conn->session,
		    GNUTLS_CRD_CERTIFICATE, x509_creds));
		gnutls_certificate_server_set_request(conn->session,
		    GNUTLS_CERT_IGNORE);
		gnutls_handshake_set_timeout(conn->session,
		    GNUTLS_DEFAULT_HANDSHAKE_TIMEOUT);
		gnutls_transport_set_int(conn->session, conn->fd);

		avl_insert(&conns, conn, where);
	}
}

static void
conn_remove_from(conn_t *conn)
{
	const list_t *l;

	ASSERT(conn != NULL);
	ASSERT(conn->logon_complete);

	l = htbl_lookup_multi(&conns_by_from, conn->from);
	ASSERT(l != NULL);
	for (void *mv = list_head(l); mv != NULL; mv = list_next(l, mv)) {
		conn_t *c = HTBL_VALUE_MULTI(mv);

		if (conn == c) {
			htbl_remove_multi(&conns_by_from, conn->from, mv);
			break;
		}
	}
	conn->logon_complete = false;
	memset(conn->from, 0, sizeof (conn->from));
}

static void
close_conn(conn_t *conn)
{
	if (conn->logon_complete)
		conn_remove_from(conn);
	avl_remove(&conns, conn);
	if (conn->tls_handshake_complete)
		gnutls_bye(conn->session, GNUTLS_SHUT_WR);
	close(conn->fd);
	gnutls_deinit(conn->session);
	free(conn->inbuf);
	free(conn->outbuf);
	free(conn);
}

static bool
process_logon_msg(conn_t *conn, const cpdlc_msg_t *msg)
{
	ASSERT(conn != NULL);
	ASSERT(msg != NULL);

	/* Authentication TODO */

	if (conn->logon_complete)
		conn_remove_from(conn);

	conn->logon_complete = true;
	lacf_strlcpy(conn->to, cpdlc_msg_get_to(msg), sizeof (conn->to));
	lacf_strlcpy(conn->from, cpdlc_msg_get_from(msg), sizeof (conn->from));

	if (conn->from[0] == '\0') {
		send_error_msg(conn, msg, "LOGON REQUIRES FROM= HEADER");
		return (false);
	}
	htbl_set(&conns_by_from, conn->from, conn);

	return (true);
}

static void
conn_send_buf(conn_t *conn, const char *buf, size_t buflen)
{
	ASSERT(conn != NULL);
	ASSERT(buf != NULL);
	ASSERT(buflen != 0);

	conn->outbuf = safe_realloc(conn->outbuf, conn->outbuf_sz + buflen + 1);
	lacf_strlcpy((char *)&conn->outbuf[conn->outbuf_sz], buf, buflen + 1);
	/* Exclude training NUL char */
	conn->outbuf_sz += buflen;
}

static void
conn_send_msg(conn_t *conn, cpdlc_msg_t *msg)
{
	unsigned l;
	char *buf;

	ASSERT(conn != NULL);
	ASSERT(msg != NULL);

	l = cpdlc_msg_encode(msg, NULL, 0);
	buf = safe_malloc(l + 1);
	cpdlc_msg_encode(msg, buf, l + 1);
	conn_send_buf(conn, buf, l);
	free(buf);
}

static void
send_error_msg(conn_t *conn, const cpdlc_msg_t *orig_msg, const char *fmt, ...)
{
	int l;
	va_list ap;
	char *buf;
	cpdlc_msg_t *msg;

	ASSERT(conn != NULL);
	ASSERT(fmt != NULL);

	va_start(ap, fmt);
	l = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	buf = safe_malloc(l + 1);
	va_start(ap, fmt);
	vsnprintf(buf, l + 1, fmt, ap);
	va_end(ap);

	if (orig_msg != NULL) {
		msg = cpdlc_msg_alloc(0, cpdlc_msg_get_min(orig_msg));
		if (cpdlc_msg_get_dl(orig_msg)) {
			cpdlc_msg_add_seg(msg, false,
			    CPDLC_UM159_ERROR_description, 0);
		} else {
			cpdlc_msg_add_seg(msg, true,
			    CPDLC_DM62_ERROR_errorinfo, 0);
		}
	} else {
		msg = cpdlc_msg_alloc(0, 0);
		cpdlc_msg_add_seg(msg, false, CPDLC_UM159_ERROR_description, 0);
	}
	cpdlc_msg_seg_set_arg(msg, 0, 0, buf, NULL);

	conn_send_msg(conn, msg);

	free(buf);
	cpdlc_msg_free(msg);
}

static bool
store_msg(const cpdlc_msg_t *msg, const char *to)
{
	int l = cpdlc_msg_encode(msg, NULL, 0);
	queued_msg_t *qmsg;
	char *buf;
	uint64_t bytes = sizeof (*qmsg) + l + 1;

	if (queued_msg_bytes + bytes > queued_msg_max_bytes) {
		fprintf(stderr, "Cannot queue message, global message queue "
		    "is completely out of space (%lld bytes)",
		    (long long)queued_msg_max_bytes);
		return (false);
	}

	qmsg = safe_calloc(1, sizeof (*qmsg));
	buf = safe_malloc(l + 1);

	cpdlc_msg_encode(msg, buf, l + 1);
	qmsg->msg = buf;
	qmsg->created = time(NULL);
	lacf_strlcpy(qmsg->from, cpdlc_msg_get_from(msg), sizeof (qmsg->from));
	lacf_strlcpy(qmsg->to, to, sizeof (qmsg->to));

	list_insert_tail(&queued_msgs, qmsg);
	queued_msg_bytes += bytes;

	return (true);
}

static void
conn_process_msg(conn_t *conn, cpdlc_msg_t *msg)
{
	char to[CALLSIGN_LEN] = { 0 };
	const list_t *l;

	ASSERT(conn != NULL);
	ASSERT(msg != NULL);

	if (!conn->logon_complete && !msg->is_logon) {
		send_error_msg(conn, msg, "LOGON REQUIRED");
		return;
	}
	if (msg->is_logon && !process_logon_msg(conn, msg))
		return;

	if (msg->to[0] != '\0') {
		lacf_strlcpy(to, msg->to, sizeof (to));
	} else if (conn->to[0] != '\0') {
		lacf_strlcpy(to, conn->to, sizeof (to));
	} else {
		send_error_msg(conn, msg, "MESSAGE MISSING TO= HEADER");
		return;
	}
	ASSERT(conn->from[0] != '\0');
	cpdlc_msg_set_from(msg, conn->from);

	l = htbl_lookup_multi(&conns_by_from, to);
	if (l == NULL || list_count(l) == 0) {
		if (!store_msg(msg, to))
			send_error_msg(conn, msg, "TOO MANY QUEUED MESSAGES");
	} else {
		for (void *mv = list_head(l); mv != NULL;
		    mv = list_next(l, mv)) {
			conn_t *tgt_conn = HTBL_VALUE_MULTI(mv);

			ASSERT(tgt_conn != NULL);
			conn_send_msg(tgt_conn, msg);
		}
	}
}

static bool
conn_process_input(conn_t *conn)
{
	int consumed_total = 0;

	ASSERT(conn != NULL);
	ASSERT(conn->inbuf_sz != 0);

	for (;;) {
		int consumed;
		cpdlc_msg_t *msg;

		if (!cpdlc_msg_decode(
		    (const char *)&conn->inbuf[consumed_total], &msg,
		    &consumed)) {
			fprintf(stderr, "Error decoding message from client\n");
			return (false);
		}
		/* No more complete messages pending? */
		if (msg == NULL)
			break;
		ASSERT(consumed != 0);
		conn_process_msg(conn, msg);
		/*
		 * If the message was queued for later delivery, it will
		 * have been encoded into a textual form. So we can get
		 * rid of the in-memory representation now.
		 */
		cpdlc_msg_free(msg);
		consumed_total += consumed;
	}
	if (consumed_total != 0) {
		ASSERT3S(consumed_total, <=, conn->inbuf_sz);
		conn->inbuf_sz -= consumed_total;
		memmove(conn->inbuf, &conn->inbuf[consumed_total],
		    conn->inbuf_sz + 1);
		conn->inbuf = realloc(conn->inbuf, conn->inbuf_sz);
	}

	return (true);
}

static bool
handle_conn_input(conn_t *conn)
{
	for (;;) {
		uint8_t buf[READ_BUF_SZ];
		size_t max_inbuf_sz = (conn->from[0] != '\0' ?
		    MAX_BUF_SZ : MAX_BUF_SZ_NO_LOGON);
		int bytes;

		if (!conn->tls_handshake_complete) {
			int error = gnutls_handshake(conn->session);

			if (error != GNUTLS_E_SUCCESS) {
				if (error == GNUTLS_E_AGAIN) {
					/* Need more data */
					return (true);
				}
				fprintf(stderr, "TLS handshake error: %s\n",
				    gnutls_strerror(error));
				close_conn(conn);
				return (false);
			}
			/* TLS handshake succeeded */
			conn->tls_handshake_complete = true;
		}

		bytes = gnutls_record_recv(conn->session, buf, sizeof (buf));

		if (bytes < 0) {
			/* Read error, or no more data pending */
			if (bytes == GNUTLS_E_AGAIN)
				return (true);
			if (!gnutls_error_is_fatal(bytes)) {
				fprintf(stderr, "Soft read error on "
				    "connection, can retry: %s\n",
				    gnutls_strerror(bytes));
				continue;
			}
			fprintf(stderr, "Fatal read error on connection: %s\n",
			    gnutls_strerror(bytes));
			close_conn(conn);
			return (false);
		}
		if (bytes == 0) {
			/* Connection closed */
			close_conn(conn);
			return (false);
		}
		for (ssize_t i = 0; i < bytes; i++) {
			/* Input sanitization, don't allow control chars */
			if (buf[i] == 0 || buf[i] > 127) {
				fprintf(stderr, "Invalid input character on "
				    "connection: data MUST be plain text\n");
				close_conn(conn);
				return (false);
			}
		}
		if (conn->inbuf_sz + bytes > max_inbuf_sz) {
			fprintf(stderr, "Input buffer overflow on connection: "
			    "wanted %d bytes, max %d bytes\n",
			    (int)(conn->inbuf_sz + bytes), (int)max_inbuf_sz);
			close_conn(conn);
			return (false);
		}

		conn->inbuf = safe_realloc(conn->inbuf,
		    conn->inbuf_sz + bytes + 1);
		memcpy(&conn->inbuf[conn->inbuf_sz], buf, bytes);
		conn->inbuf_sz += bytes;
		conn->inbuf[conn->inbuf_sz] = '\0';

		if (!conn_process_input(conn)) {
			close_conn(conn);
			return (false);
		}
	}
}

static bool
handle_conn_output(conn_t *conn)
{
	int bytes;

	ASSERT(conn != NULL);
	ASSERT(conn->outbuf != NULL);

	bytes = gnutls_record_send(conn->session, conn->outbuf,
	    conn->outbuf_sz);
	if (bytes < 0) {
		if (bytes != GNUTLS_E_AGAIN) {
			if (gnutls_error_is_fatal(bytes)) {
				fprintf(stderr, "Fatal send error on "
				    "connection: %s\n", gnutls_strerror(bytes));
				close_conn(conn);
				return (false);
			}
			fprintf(stderr, "Soft send error on connection: %s\n",
			    gnutls_strerror(bytes));
		}
	} else if (bytes > 0) {
		if ((ssize_t)conn->outbuf_sz > bytes) {
			memmove(conn->outbuf, &conn->outbuf[bytes],
			    (conn->outbuf_sz - bytes) + 1);
			conn->outbuf = safe_realloc(conn->outbuf,
			    (conn->outbuf_sz - bytes) + 1);
			conn->outbuf_sz -= bytes;
		} else {
			free(conn->outbuf);
			conn->outbuf = NULL;
			conn->outbuf_sz = 0;
		}
	}

	return (true);
}

static void
poll_sockets(void)
{
	unsigned sock_nr = 0;
	unsigned num_pfds = avl_numnodes(&listen_socks) + avl_numnodes(&conns);
	struct pollfd *pfds = safe_calloc(num_pfds, sizeof (*pfds));
	int poll_res, polls_seen;

	for (listen_sock_t *ls = avl_first(&listen_socks); ls != NULL;
	    ls = AVL_NEXT(&listen_socks, ls), sock_nr++) {
		pfds[sock_nr].fd = ls->fd;
		pfds[sock_nr].events = POLLIN;
	}
	for (conn_t *conn = avl_first(&conns); conn != NULL;
	    conn = AVL_NEXT(&conns, conn), sock_nr++) {
		pfds[sock_nr].fd = conn->fd;
		pfds[sock_nr].events = POLLIN;
		if (conn->outbuf != NULL)
			pfds[sock_nr].events |= POLLOUT;
	}
	ASSERT3U(sock_nr, ==, num_pfds);

	poll_res = poll(pfds, num_pfds, POLL_TIMEOUT);
	if (poll_res == -1 && errno != EINTR) {
		fprintf(stderr, "Error polling on sockets: %s\n",
		    strerror(errno));
		free(pfds);
		return;
	}
	if (poll_res == 0) {
		/* Poll timeout, respin another loop */
		free(pfds);
		return;
	}

	polls_seen = 0;
	sock_nr = 0;
	for (listen_sock_t *ls = avl_first(&listen_socks); ls != NULL;
	    ls = AVL_NEXT(&listen_socks, ls), sock_nr++) {
		if (pfds[sock_nr].revents & (POLLIN | POLLPRI)) {
			handle_accepts(ls);
			polls_seen++;
			if (polls_seen == poll_res)
				goto out;
		}
	}
	for (conn_t *conn = avl_first(&conns), *next_conn = NULL;
	    conn != NULL; conn = next_conn, sock_nr++) {
		/*
		 * Grab the next connection handle now in case
		 * the connection gets closed due to EOF or errors.
		 */
		next_conn = AVL_NEXT(&conns, conn);
		if (pfds[sock_nr].revents & (POLLIN | POLLOUT)) {
			if (pfds[sock_nr].revents & POLLIN)
				(void) handle_conn_input(conn);
			if (conn->outbuf != NULL &&
			    (pfds[sock_nr].revents & POLLOUT))
				(void) handle_conn_output(conn);
			polls_seen++;
			if (polls_seen == poll_res)
				goto out;
		}
	}
out:

	free(pfds);
}

static void
dequeue_msg(queued_msg_t *qmsg)
{
	uint64_t bytes = sizeof (*qmsg) + strlen(qmsg->msg) + 1;

	ASSERT3U(queued_msg_bytes, >=, bytes);
	queued_msg_bytes -= bytes;
	list_remove(&queued_msgs, qmsg);
	free(qmsg->msg);
	free(qmsg);
	if (list_count(&queued_msgs) == 0)
		ASSERT0(queued_msg_bytes);
}

static void
handle_queued_msgs(void)
{
	time_t now = time(NULL);

	for (queued_msg_t *qmsg = list_head(&queued_msgs), *next_qmsg = NULL;
	    qmsg != NULL; qmsg = next_qmsg) {
		const list_t *l;

		next_qmsg = list_next(&queued_msgs, qmsg);

		l = htbl_lookup_multi(&conns_by_from, qmsg->to);
		if (l != NULL && list_count(l) != 0) {
			for (void *mv = list_head(l); mv != NULL;
			    mv = list_next(l, mv)) {
				conn_t *conn = HTBL_VALUE_MULTI(mv);
				conn_send_buf(conn, qmsg->msg,
				strlen(qmsg->msg));
			}
			dequeue_msg(qmsg);
		} else if (now - qmsg->created > QUEUED_MSG_TIMEOUT) {
			dequeue_msg(qmsg);
		}
	}
}

static void
close_blocked_conns(void)
{
	/*
	 * Run through existing connections and close ones which are now
	 * on the blocklist.
	 */
	for (conn_t *conn = avl_first(&conns), *conn_next = NULL; conn != NULL;
	    conn = conn_next) {
		conn_next = AVL_NEXT(&conns, conn);
		if (!blocklist_check(conn->addr, conn->addr_len,
		    conn->addr_family)) {
			close_conn(conn);
		}
	}
}

static bool
tls_init(void)
{
#define	CHECKFILE(__filename) \
	do { \
		struct stat st; \
		if (stat((__filename), &st) != 0) { \
			fprintf(stderr, "Can't stat %s: %s\n", \
			    (__filename), strerror(errno)); \
			gnutls_global_deinit(); \
			return (false); \
		} \
	} while (0)
#define	CHECK(op) \
	do { \
		int error = (op); \
		if (error != GNUTLS_E_SUCCESS) { \
			fprintf(stderr, #op " failed: %s\n", \
			    gnutls_strerror(error)); \
			gnutls_global_deinit(); \
			return (false); \
		} \
	} while (0)

	CHECK(gnutls_global_init());
	CHECK(gnutls_certificate_allocate_credentials(&x509_creds));
	if (cafile[0] != '\0') {
		CHECKFILE(cafile);
		CHECK(gnutls_certificate_set_x509_trust_file(x509_creds,
		    cafile, GNUTLS_X509_FMT_PEM));
	}
	CHECKFILE(keyfile);
	CHECKFILE(certfile);
	CHECK(gnutls_certificate_set_x509_key_file(x509_creds, certfile,
	    keyfile, GNUTLS_X509_FMT_PEM));
        CHECK(gnutls_priority_init(&prio_cache, NULL, NULL));
#if	GNUTLS_VERSION_NUMBER >= 0x030506
	gnutls_certificate_set_known_dh_params(x509_creds,
	    GNUTLS_SEC_PARAM_HIGH);
#endif	/* GNUTLS_VERSION_NUMBER */

	return (true);
#undef	CHECK
#undef	CHECKFILE
}

static void
tls_fini(void)
{
	gnutls_certificate_free_credentials(x509_creds);
	gnutls_priority_deinit(prio_cache);
	gnutls_global_deinit();
}

int
main(int argc, char *argv[])
{
	int opt;
	const char *conf_path = NULL;

	log_init((logfunc_t)puts, "cpdlcd");
	crc64_init();
	curl_global_init(CURL_GLOBAL_ALL);

	/* Default certificate names */
	lacf_strlcpy(keyfile, "cpdlcd_key.pem", sizeof (keyfile));
	lacf_strlcpy(certfile, "cpdlcd_cert.pem", sizeof (certfile));

	while ((opt = getopt(argc, argv, "hc:dp:")) != -1) {
		switch (opt) {
		case 'h':
			print_usage(argv[0], stdout);
			return (0);
		case 'c':
			conf_path = optarg;
			break;
		case 'd':
			background = false;
			break;
		case 'p':
			default_port = atoi(optarg);
			if (default_port <= 0 || default_port > UINT16_MAX) {
				fprintf(stderr, "Invalid port number, must be "
				    "an integer between 0 and %d", UINT16_MAX);
				return (1);
			}
			break;
		default:
			print_usage(argv[0], stderr);
			return (1);
		}
	}

	init_structs();
	if (background && !daemonize(true, true))
		return (1);
	if ((conf_path != NULL && !parse_config(conf_path)) ||
	    (conf_path == NULL && !auto_config())) {
		return (1);
	}
	if (!tls_init())
		return (1);

	while (!do_shutdown) {
		poll_sockets();
		handle_queued_msgs();
		if (blocklist_refresh())
			close_blocked_conns();
	}

	tls_fini();
	fini_structs();
	curl_global_cleanup();

	return (0);
}