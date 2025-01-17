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

#include <stdlib.h>

#include "cpdlc_alloc.h"
#include "cpdlc_assert.h"
#include "cpdlc_msglist.h"
#include "cpdlc_thread.h"
#include "minilist.h"

typedef struct msg_bucket_s {
	cpdlc_msg_t		*msg;
	cpdlc_msg_token_t	tok;
	bool			sent;
	list_node_t		node;
	unsigned		hours;
	unsigned		mins;
	time_t			time;
} msg_bucket_t;

typedef struct msg_thr_s {
	cpdlc_msg_thr_id_t	thr_id;
	cpdlc_msg_thr_status_t	status;
	list_t			buckets;
	list_node_t		node;
	bool			dirty;
} msg_thr_t;

struct cpdlc_msglist_s {
	cpdlc_client_t			*cl;
	mutex_t				lock;

	list_t				thr;

	unsigned			min;
	unsigned			mrn;
	cpdlc_msg_thr_id_t		next_thr_id;

	cpdlc_msglist_update_cb_t	update_cb;
	void				*userinfo;

	cpdlc_get_time_func_t		get_time_func;
};

static msg_thr_t *msglist_send_impl(cpdlc_msglist_t *msglist,
    cpdlc_msg_t *msg, cpdlc_msg_thr_id_t thr_id);

static bool
msg_is_dl_req(const cpdlc_msg_t *msg)
{
	int msg_type;
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	msg_type = msg->segs[0].info->msg_type;
	return ((msg_type >= CPDLC_DM6_REQ_alt &&
	    msg_type <= CPDLC_DM27_REQ_WX_DEVIATION_UP_TO_dir_dist_OF_ROUTE) ||
	    (msg_type >= CPDLC_DM49_WHEN_CAN_WE_EXPCT_spd &&
	    msg_type <= CPDLC_DM54_WHEN_CAN_WE_EXPECT_CRZ_CLB_TO_alt) ||
	    msg_type == CPDLC_DM70_REQ_HDG_deg ||
	    msg_type == CPDLC_DM71_REQ_GND_TRK_deg);
}

static bool
msg_dl_req_resp(const cpdlc_msg_t *msg)
{
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	ASSERT(msg->segs[0].info->is_dl);
	return (msg->segs[0].info->resp == CPDLC_RESP_Y);
}

static bool
msg_is_ul_req(const cpdlc_msg_t *msg)
{
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	return (msg->segs[0].info->resp == CPDLC_RESP_WU ||
	    msg->segs[0].info->resp == CPDLC_RESP_AN ||
	    msg->segs[0].info->resp == CPDLC_RESP_NE);
}

static bool
msg_is_stby(const cpdlc_msg_t *msg)
{
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	return ((msg->segs[0].info->is_dl &&
	    msg->segs[0].info->msg_type == CPDLC_DM2_STANDBY) ||
	    (!msg->segs[0].info->is_dl &&
	    msg->segs[0].info->msg_type == CPDLC_UM1_STANDBY));
}

static bool
msg_is_accept(const cpdlc_msg_t *msg)
{
	int msg_type;
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	msg_type = msg->segs[0].info->msg_type;
	return ((msg->segs[0].info->is_dl &&
	    (msg_type == CPDLC_DM0_WILCO || msg_type == CPDLC_DM4_AFFIRM)) ||
	    (!msg->segs[0].info->is_dl && msg_type == CPDLC_UM4_AFFIRM));
}

static bool
msg_is_reject(const cpdlc_msg_t *msg)
{
	int msg_type;
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	msg_type = msg->segs[0].info->msg_type;
	return ((msg->segs[0].info->is_dl &&
	    (msg_type == CPDLC_DM1_UNABLE || msg_type == CPDLC_DM5_NEGATIVE ||
	    msg_type == CPDLC_DM62_ERROR_errorinfo)) ||
	    (!msg->segs[0].info->is_dl &&
	    (msg_type == CPDLC_UM0_UNABLE || msg_type == CPDLC_UM5_NEGATIVE ||
	    msg_type == CPDLC_UM159_ERROR_description)));
}

static bool
is_error_msg(const cpdlc_msg_t *msg)
{
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	return ((msg->segs[0].info->is_dl &&
	    msg->segs[0].info->msg_type == CPDLC_DM62_ERROR_errorinfo) ||
	    (!msg->segs[0].info->is_dl &&
	    msg->segs[0].info->msg_type == CPDLC_UM159_ERROR_description));
}

static bool
msg_is_rgr(const cpdlc_msg_t *msg)
{
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	return ((msg->segs[0].info->is_dl &&
	    msg->segs[0].info->msg_type == CPDLC_DM3_ROGER) ||
	    (!msg->segs[0].info->is_dl &&
	    msg->segs[0].info->msg_type == CPDLC_UM3_ROGER));
}

static bool
msg_is_link_mgmt(const cpdlc_msg_t *msg)
{
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	return (!msg->segs[0].info->is_dl &&
	    (msg->segs[0].info->msg_type == CPDLC_UM161_END_SVC ||
	    msg->segs[0].info->msg_type == CPDLC_UM160_NEXT_DATA_AUTHORITY_id));
}

static bool
is_disregard_msg(const cpdlc_msg_t *msg)
{
	ASSERT(msg != NULL);
	ASSERT(msg->segs[0].info != NULL);
	return (!msg->segs[0].info->is_dl &&
	    msg->segs[0].info->msg_type == CPDLC_UM168_DISREGARD);
}

static bool
thr_status_is_final(cpdlc_msg_thr_status_t st)
{
	return (st == CPDLC_MSG_THR_CLOSED || st == CPDLC_MSG_THR_ACCEPTED ||
	    st == CPDLC_MSG_THR_REJECTED || st == CPDLC_MSG_THR_TIMEDOUT ||
	    st == CPDLC_MSG_THR_DISREGARD || st == CPDLC_MSG_THR_FAILED ||
	    st == CPDLC_MSG_THR_ERROR || st == CPDLC_MSG_THR_CONN_ENDED);
}

static unsigned
thr_get_timeout(msg_thr_t *thr)
{
	unsigned timeout = UINT32_MAX;

	ASSERT(thr != NULL);
	for (msg_bucket_t *bucket = list_head(&thr->buckets); bucket != NULL;
	    bucket = list_next(&thr->buckets, bucket)) {
		const cpdlc_msg_t *msg = bucket->msg;

		ASSERT(msg != NULL);
		for (unsigned i = 0, n = cpdlc_msg_get_num_segs(msg); i < n;
		    i++) {
			ASSERT(msg->segs[i].info != NULL);
			if (msg->segs[i].info->timeout != 0 &&
			    msg->segs[i].info->timeout < timeout) {
				timeout = msg->segs[i].info->timeout;
			}
		}
	}

	return (timeout < UINT32_MAX ? timeout : 0);
}

static void
thr_status_upd(cpdlc_msglist_t *msglist, msg_thr_t *thr)
{
	msg_bucket_t *first = list_head(&thr->buckets);
	msg_bucket_t *last = list_tail(&thr->buckets);
	time_t now = time(NULL);
	unsigned timeout;

	if (thr_status_is_final(thr->status))
		return;

	timeout = thr_get_timeout(thr);

	if (first == last && first->sent && !msg_dl_req_resp(first->msg)) {
		thr->status = CPDLC_MSG_THR_CLOSED;
	} else if (last->sent && msg_is_dl_req(last->msg)) {
		switch (cpdlc_client_get_msg_status(msglist->cl, last->tok)) {
		case CPDLC_MSG_STATUS_SENDING:
			thr->status = CPDLC_MSG_THR_PENDING;
			break;
		case CPDLC_MSG_STATUS_SEND_FAILED:
			thr->status = CPDLC_MSG_THR_FAILED;
			break;
		default:
			thr->status = CPDLC_MSG_THR_OPEN;
			break;
		}
	} else if (msg_is_stby(last->msg)) {
		thr->status = CPDLC_MSG_THR_STANDBY;
	} else if (msg_is_accept(last->msg)) {
		thr->status = CPDLC_MSG_THR_ACCEPTED;
	} else if (msg_is_reject(last->msg)) {
		thr->status = CPDLC_MSG_THR_REJECTED;
	} else if (msg_is_rgr(last->msg) || msg_is_link_mgmt(last->msg)) {
		thr->status = CPDLC_MSG_THR_CLOSED;
	} else if (msg_is_ul_req(last->msg) &&
	    thr->status != CPDLC_MSG_THR_STANDBY && timeout != 0 &&
	    now - last->time > timeout) {
		cpdlc_msg_t *msg = cpdlc_msg_alloc(CPDLC_PKT_CPDLC);
		cpdlc_msg_set_mrn(msg, cpdlc_msg_get_min(last->msg));
		cpdlc_msg_add_seg(msg, true, CPDLC_DM62_ERROR_errorinfo, 0);
		cpdlc_msg_seg_set_arg(msg, 0, 0, "TIMEDOUT", NULL);
		msglist_send_impl(msglist, msg, thr->thr_id);
		thr->status = CPDLC_MSG_THR_TIMEDOUT;
	} else if (is_disregard_msg(last->msg)) {
		thr->status = CPDLC_MSG_THR_DISREGARD;
	} else if (is_error_msg(last->msg)) {
		thr->status = CPDLC_MSG_THR_ERROR;
	} else if (cpdlc_client_get_logon_status(msglist->cl, NULL) !=
	    CPDLC_LOGON_COMPLETE) {
		thr->dirty = false;
		thr->status = CPDLC_MSG_THR_CONN_ENDED;
	}
}

static void
dfl_get_time_func(void *unused, unsigned *hours, unsigned *mins)
{
	time_t now = time(NULL);
	const struct tm *tm = localtime(&now);
	UNUSED(unused);
	ASSERT(hours != NULL);
	ASSERT(mins != NULL);
	*hours = tm->tm_hour;
	*mins = tm->tm_min;
}

static msg_thr_t *
find_msg_thr(cpdlc_msglist_t *msglist, cpdlc_msg_thr_id_t thr_id)
{
	ASSERT(msglist != NULL);

	if (thr_id != CPDLC_NO_MSG_THR_ID) {
		for (msg_thr_t *thr = list_head(&msglist->thr); thr != NULL;
		    thr = list_next(&msglist->thr, thr)) {
			if (thr->thr_id == thr_id)
				return (thr);
		}
		VERIFY_MSG(0, "Invalid message thread ID %x", thr_id);
	} else {
		msg_thr_t *thr = safe_calloc(1, sizeof (*thr));
		thr->thr_id = msglist->next_thr_id++;
		list_create(&thr->buckets, sizeof (msg_bucket_t),
		    offsetof(msg_bucket_t, node));
		list_insert_head(&msglist->thr, thr);
		return (thr);
	}
}

static void
free_msg_thr(msg_thr_t *thr)
{
	msg_bucket_t *bucket;

	ASSERT(thr != NULL);

	while ((bucket = list_remove_head(&thr->buckets)) != NULL) {
		ASSERT(bucket->msg != NULL);
		cpdlc_msg_free(bucket->msg);
		free(bucket);
	}
	list_destroy(&thr->buckets);
	free(thr);
}

static bool
msg_matches_bucket(const cpdlc_msg_t *msg, const msg_bucket_t *bucket)
{
	unsigned min, mrn;

	ASSERT(msg != NULL);
	ASSERT(bucket != NULL);
	ASSERT(bucket->msg != NULL);
	ASSERT(msg->segs[0].info != NULL);

	min = cpdlc_msg_get_min(bucket->msg);
	mrn = cpdlc_msg_get_mrn(msg);

	if (is_disregard_msg(msg))
		return (!bucket->sent && min == mrn);
	else
		return (bucket->sent && min == mrn);
}

msg_thr_t *
msg_thr_find_by_mrn(cpdlc_msglist_t *msglist, const cpdlc_msg_t *msg)
{
	ASSERT(msglist != NULL);
	ASSERT(msg != NULL);

	if (cpdlc_msg_get_mrn(msg) == CPDLC_INVALID_MSG_SEQ_NR)
		return (NULL);
	for (msg_thr_t *thr = list_tail(&msglist->thr); thr != NULL;
	    thr = list_prev(&msglist->thr, thr)) {
		/*
		 * Skip manually closed threads. This allows the FMS
		 * to force the message list to receive all uplink
		 * messages into new threads.
		 */
		if (thr->status == CPDLC_MSG_THR_CLOSED)
			continue;
		for (msg_bucket_t *bucket = list_tail(&thr->buckets);
		    bucket != NULL; bucket = list_prev(&thr->buckets, bucket)) {
			ASSERT(bucket->msg != NULL);
			if (msg_matches_bucket(msg, bucket))
				return (thr);
		}
	}
	return (NULL);
}

static void
msg_recv_cb(cpdlc_client_t *cl)
{
	cpdlc_msglist_t *msglist;
	cpdlc_msg_t *msg;
	cpdlc_msg_thr_id_t *upd_thrs = NULL;
	unsigned num_upd_thrs = 0;
	cpdlc_msglist_update_cb_t update_cb;

	ASSERT(cl != NULL);
	msglist = cpdlc_client_get_cb_userinfo(cl);
	ASSERT(msglist != NULL);

	mutex_enter(&msglist->lock);

	update_cb = msglist->update_cb;

	while ((msg = cpdlc_client_recv_msg(cl)) != NULL) {
		msg_thr_t *thr = msg_thr_find_by_mrn(msglist, msg);
		msg_bucket_t *bucket;

		if (thr == NULL)
			thr = find_msg_thr(msglist, CPDLC_NO_MSG_THR_ID);
		bucket = safe_calloc(1, sizeof (*bucket));
		bucket->msg = msg;
		bucket->tok = CPDLC_INVALID_MSG_TOKEN;
		ASSERT(msglist->get_time_func != NULL);
		msglist->get_time_func(msglist->userinfo, &bucket->hours,
		    &bucket->mins);
		bucket->time = time(NULL);
		thr->dirty = true;

		list_insert_tail(&thr->buckets, bucket);
		thr_status_upd(msglist, thr);

		if (update_cb != NULL) {
			upd_thrs = safe_realloc(upd_thrs, (num_upd_thrs + 1) *
			    sizeof (*upd_thrs));
			upd_thrs[num_upd_thrs] = thr->thr_id;
			num_upd_thrs++;
		}
	}
	mutex_exit(&msglist->lock);
	/*
	 * Call this outside of locking context to avoid locking inversions.
	 */
	if (update_cb != NULL) {
		update_cb(msglist, upd_thrs, num_upd_thrs);
		free(upd_thrs);
	}
}

cpdlc_msglist_t *
cpdlc_msglist_alloc(cpdlc_client_t *cl)
{
	cpdlc_msglist_t *msglist = safe_calloc(1, sizeof (*msglist));

	ASSERT(cl != NULL);

	cpdlc_client_set_msg_recv_cb(cl, msg_recv_cb);
	cpdlc_client_set_cb_userinfo(cl, msglist);

	mutex_init(&msglist->lock);
	list_create(&msglist->thr, sizeof (msg_thr_t),
	    offsetof(msg_thr_t, node));
	msglist->cl = cl;
	msglist->get_time_func = dfl_get_time_func;

	return (msglist);
}

void
cpdlc_msglist_free(cpdlc_msglist_t *msglist)
{
	msg_thr_t *thr;

	ASSERT(msglist != NULL);

	while ((thr = list_remove_head(&msglist->thr)) != NULL)
		free_msg_thr(thr);
	mutex_destroy(&msglist->lock);
	free(msglist);
}

void
cpdlc_msglist_update(cpdlc_msglist_t *msglist)
{
	ASSERT(msglist != NULL);

	mutex_enter(&msglist->lock);

	for (msg_thr_t *thr = list_head(&msglist->thr); thr != NULL;
	    thr = list_next(&msglist->thr, thr))
		thr_status_upd(msglist, thr);

	mutex_exit(&msglist->lock);
}

static msg_thr_t *
msglist_send_impl(cpdlc_msglist_t *msglist, cpdlc_msg_t *msg,
    cpdlc_msg_thr_id_t thr_id)
{
	msg_thr_t		*thr;
	msg_bucket_t		*bucket;

	ASSERT(msglist != NULL);
	ASSERT(msg != NULL);

	thr = find_msg_thr(msglist, thr_id);
	if (thr_id == CPDLC_NO_MSG_THR_ID)
		thr->status = CPDLC_MSG_THR_OPEN;
	else
		ASSERT(!thr_status_is_final(thr->status));
	thr_id = thr->thr_id;

	/* Assign the appropriate MIN and MRN flags */
	for (msg_bucket_t *bucket = list_tail(&thr->buckets); bucket != NULL;
	    bucket = list_prev(&thr->buckets, bucket)) {
		if (cpdlc_msg_get_dl(bucket->msg) != cpdlc_msg_get_dl(msg)) {
			cpdlc_msg_set_mrn(msg, cpdlc_msg_get_min(bucket->msg));
			break;
		}
	}
	cpdlc_msg_set_min(msg, msglist->min++);

	bucket = safe_calloc(1, sizeof (*bucket));
	bucket->msg = msg;
	bucket->tok = cpdlc_client_send_msg(msglist->cl, msg);
	bucket->sent = true;
	ASSERT(msglist->get_time_func != NULL);
	msglist->get_time_func(msglist->userinfo, &bucket->hours,
	    &bucket->mins);
	bucket->time = time(NULL);
	list_insert_tail(&thr->buckets, bucket);

	return (thr);
}

cpdlc_msg_thr_id_t
cpdlc_msglist_send(cpdlc_msglist_t *msglist, cpdlc_msg_t *msg,
    cpdlc_msg_thr_id_t thr_id)
{
	msg_thr_t *thr;

	ASSERT(msglist != NULL);
	ASSERT(msg != NULL);

	mutex_enter(&msglist->lock);
	thr = msglist_send_impl(msglist, msg, thr_id);
	thr_id = thr->thr_id;
	thr_status_upd(msglist, thr);
	mutex_exit(&msglist->lock);

	return (thr_id);
}

void
cpdlc_msglist_get_thr_ids(cpdlc_msglist_t *msglist, bool ignore_closed,
    cpdlc_msg_thr_id_t *thr_ids, unsigned *cap)
{
	unsigned thr_i = 0;

	ASSERT(msglist != NULL);
	ASSERT(cap != NULL);

	mutex_enter(&msglist->lock);
	for (msg_thr_t *thr = list_head(&msglist->thr); thr != NULL;
	    thr = list_next(&msglist->thr, thr)) {
		if (ignore_closed && !thr->dirty &&
		    thr_status_is_final(thr->status))
			continue;
		if (thr_i < *cap) {
			ASSERT(thr_ids != NULL);
			thr_ids[thr_i] = thr->thr_id;
		}
		thr_i++;
	}
	mutex_exit(&msglist->lock);

	if (thr_ids == NULL)
		*cap = thr_i;
	else
		*cap = MIN(*cap, thr_i);
}

cpdlc_msg_thr_status_t
cpdlc_msglist_get_thr_status(cpdlc_msglist_t *msglist,
    cpdlc_msg_thr_id_t thr_id, bool *dirty)
{
	msg_thr_t		*thr;
	cpdlc_msg_thr_status_t	status;

	ASSERT(msglist != NULL);
	ASSERT(thr_id != CPDLC_NO_MSG_THR_ID);

	mutex_enter(&msglist->lock);
	thr = find_msg_thr(msglist, thr_id);
	status = thr->status;
	if (dirty != NULL)
		*dirty = thr->dirty;
	mutex_exit(&msglist->lock);

	return (status);
}

void
cpdlc_msglist_thr_mark_seen(cpdlc_msglist_t *msglist, cpdlc_msg_thr_id_t thr_id)
{
	msg_thr_t *thr;

	ASSERT(msglist != NULL);
	ASSERT(thr_id != CPDLC_NO_MSG_THR_ID);

	mutex_enter(&msglist->lock);
	thr = find_msg_thr(msglist, thr_id);
	thr->dirty = false;
	mutex_exit(&msglist->lock);
}

unsigned
cpdlc_msglist_get_thr_msg_count(cpdlc_msglist_t *msglist,
    cpdlc_msg_thr_id_t thr_id)
{
	unsigned count;
	msg_thr_t *thr;

	ASSERT(msglist != NULL);
	ASSERT(thr_id != CPDLC_NO_MSG_THR_ID);

	mutex_enter(&msglist->lock);
	thr = find_msg_thr(msglist, thr_id);
	count = list_count(&thr->buckets);
	mutex_exit(&msglist->lock);

	return (count);
}

void
cpdlc_msglist_get_thr_msg(cpdlc_msglist_t *msglist, cpdlc_msg_thr_id_t thr_id,
    unsigned msg_nr, const cpdlc_msg_t **msg_p, cpdlc_msg_token_t *token_p,
    unsigned *hours_p, unsigned *mins_p, bool *is_sent_p)
{
	msg_thr_t *thr;
	msg_bucket_t *bucket;

	ASSERT(msglist != NULL);
	ASSERT(thr_id != CPDLC_NO_MSG_THR_ID);

	mutex_enter(&msglist->lock);

	thr = find_msg_thr(msglist, thr_id);
	ASSERT3U(msg_nr, <, list_count(&thr->buckets));
	bucket = list_head(&thr->buckets);
	for (unsigned i = 0; i < msg_nr; i++)
		bucket = list_next(&thr->buckets, bucket);
	ASSERT(bucket != NULL);
	if (msg_p != NULL)
		*msg_p = bucket->msg;
	if (token_p != NULL)
		*token_p = bucket->tok;
	if (hours_p != NULL)
		*hours_p = bucket->hours;
	if (mins_p != NULL)
		*mins_p = bucket->mins;
	if (is_sent_p != NULL)
		*is_sent_p = bucket->sent;

	mutex_exit(&msglist->lock);
}

void
cpdlc_msglist_remove_thr(cpdlc_msglist_t *msglist, cpdlc_msg_thr_id_t thr_id)
{
	msg_thr_t *thr;

	ASSERT(msglist != NULL);
	ASSERT(thr_id != CPDLC_NO_MSG_THR_ID);

	mutex_enter(&msglist->lock);
	thr = find_msg_thr(msglist, thr_id);
	list_remove(&msglist->thr, thr);
	mutex_exit(&msglist->lock);

	free_msg_thr(thr);
}

bool
cpdlc_msglist_thr_is_done(cpdlc_msglist_t *msglist, cpdlc_msg_thr_id_t thr_id)
{
	msg_thr_t *thr;
	bool result;

	ASSERT(msglist != NULL);
	ASSERT(thr_id != CPDLC_NO_MSG_THR_ID);

	mutex_enter(&msglist->lock);
	thr = find_msg_thr(msglist, thr_id);
	result = thr_status_is_final(thr->status);
	mutex_exit(&msglist->lock);

	return (result);
}

void
cpdlc_msglist_thr_close(cpdlc_msglist_t *msglist, cpdlc_msg_thr_id_t thr_id)
{
	msg_thr_t *thr;

	ASSERT(msglist != NULL);
	ASSERT(thr_id != CPDLC_NO_MSG_THR_ID);

	mutex_enter(&msglist->lock);
	thr = find_msg_thr(msglist, thr_id);
	if (!thr_status_is_final(thr->status))
		thr->status = CPDLC_MSG_THR_CLOSED;
	mutex_exit(&msglist->lock);
}

void
cpdlc_msglist_set_userinfo(cpdlc_msglist_t *msglist, void *userinfo)
{
	ASSERT(msglist != NULL);
	mutex_enter(&msglist->lock);
	msglist->userinfo = userinfo;
	mutex_exit(&msglist->lock);
}

void *
cpdlc_msglist_get_userinfo(cpdlc_msglist_t *msglist)
{
	ASSERT(msglist != NULL);
	/* Reading a pointer is atomic, no locking req'd */
	return (msglist->userinfo);
}

void
cpdlc_msglist_set_update_cb(cpdlc_msglist_t *msglist,
    cpdlc_msglist_update_cb_t update_cb)
{
	ASSERT(msglist != NULL);
	mutex_enter(&msglist->lock);
	msglist->update_cb = update_cb;
	mutex_exit(&msglist->lock);
}

void
cpdlc_msglist_set_get_time_func(cpdlc_msglist_t *msglist,
    cpdlc_get_time_func_t func)
{
	ASSERT(msglist != NULL);
	ASSERT(func != NULL);
	mutex_enter(&msglist->lock);
	msglist->get_time_func = func;
	mutex_exit(&msglist->lock);
}
