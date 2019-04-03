/**
 * (C) Copyright 2016-2019 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

#include "dfuse_common.h"
#include "dfuse.h"
#include "dfuse_pool.h"

int
dfuse_fs_resend(struct dfuse_request *request);

bool
dfuse_gen_cb(struct dfuse_request *request)
{
	struct dfuse_status_out *out = crt_reply_get(request->rpc);

	IOC_REQUEST_RESOLVE(request, out);
	if (request->rc) {
		IOC_REPLY_ERR(request, request->rc);
		D_GOTO(out, 0);
	}

	IOC_REPLY_ZERO(request);

out:
	/* Clean up the two refs this code holds on the rpc */
	crt_req_decref(request->rpc);
	crt_req_decref(request->rpc);

	D_FREE(request);
	return false;
}

/* A generic callback function to handle completion of RPCs sent from FUSE,
 * and replay the RPC to a different end point in case the target has been
 * evicted (denoted by an "Out Of Group" return code). For all other failures
 * and in case of success, it invokes a custom handler (if defined).
 */
static void
generic_cb(const struct crt_cb_info *cb_info)
{
	struct dfuse_request		*request = cb_info->cci_arg;
	struct dfuse_projection_info	*fsh = request->fsh;
	struct dfuse_inode_entry	*ir_inode = NULL;
	bool				keep_ref;

	D_ASSERT(request->ir_rs == RS_RESET);
	request->ir_rs = RS_LIVE;

	IOF_TRACE_INFO(request, "cci_rc %d -%s",
		       cb_info->cci_rc, d_errstr(cb_info->cci_rc));

	if (request->ir_ht == RHS_INODE) {
		ir_inode = request->ir_inode;
	}

	keep_ref = request->ir_api->on_result(request);

	if (ir_inode && !keep_ref) {
		d_hash_rec_decref(&fsh->inode_ht, &ir_inode->ie_htl);
	}

}

/*
 * Wrapper function that is called from FUSE to send RPCs. The idea is to
 * decouple the FUSE implementation from the actual sending of RPCs. The
 * FUSE callbacks only need to specify the inputs and outputs for the RPC,
 * without bothering about how RPCs are sent. This function is also intended
 * for abstracting various other features related to RPCs such as fail-over
 * and load balance, at the same time preventing code duplication.
 *
 */
int
dfuse_fs_send(struct dfuse_request *request)
{
	int rc;

	D_ASSERT(request->ir_api->on_result);
	/* If the API has passed in a simple inode number then translate it
	 * to either root, or do a hash table lookup on the inode number.
	 * Keep a reference on the inode open which will be dropped after
	 * a call to on_result().
	 */
	if (request->ir_ht == RHS_INODE_NUM) {

		D_ASSERT(request->ir_api->have_gah);

		if (request->ir_inode_num == 1) {
			request->ir_ht = RHS_ROOT;
		} else {
			rc = find_inode(request);
			if (rc != 0) {
				D_GOTO(err, 0);
			}
			request->ir_ht = RHS_INODE;
		}
	}
	rc = dfuse_fs_resend(request);
	if (rc) {
		D_GOTO(err, 0);
	}
	return 0;
err:
	IOF_TRACE_ERROR(request, "Could not send rpc, rc = %d", rc);

	return rc;
}

int
dfuse_fs_resend(struct dfuse_request *request)
{
	struct dfuse_projection_info	*fs_handle = request->fsh;
	crt_endpoint_t			ep;
	int				ret;
	int				rc;

	if (request->ir_api->have_gah) {
		void *in = crt_req_get(request->rpc);
		struct ios_gah *gah = in + request->ir_api->gah_offset;

		IOF_TRACE_DEBUG(request,
				"loading gah from %d %p", request->ir_ht,
				request->ir_inode);

		D_MUTEX_LOCK(&request->fsh->gah_lock);

		switch (request->ir_ht) {
		case RHS_ROOT:
			*gah = request->fsh->gah;
			break;
		case RHS_INODE:
			*gah = request->ir_inode->gah;
			break;
		case RHS_FILE:
			*gah = request->ir_file->common.gah;
			break;
		case RHS_DIR:
			*gah = request->ir_dir->gah;
			break;
		default:
			IOF_TRACE_ERROR(request,
					"Invalid request type %d",
					request->ir_ht);
			D_MUTEX_UNLOCK(&request->fsh->gah_lock);
			D_GOTO(err, ret = EIO);
		}

		D_MUTEX_UNLOCK(&request->fsh->gah_lock);
		IOF_TRACE_DEBUG(request, GAH_PRINT_STR, GAH_PRINT_VAL(*gah));
	}

	ep.ep_tag = 0;
	ep.ep_grp = fs_handle->proj.grp->dest_grp;

	/* Pick an appropriate rank, for most cases this is the root of the GAH
	 * however if that is not known then send to the PSR
	 */
	switch (request->ir_ht) {
	case RHS_INODE:
		ep.ep_rank = request->ir_inode->gah.root;
		break;
	case RHS_FILE:
		ep.ep_rank = request->ir_file->common.gah.root;
		break;
	case RHS_DIR:
		ep.ep_rank = request->ir_dir->gah.root;
		break;
	case RHS_ROOT:
	default:
		ep.ep_rank = fs_handle->gah.root;
	}

	/* Defer clean up until the output is copied. */
	rc = crt_req_set_endpoint(request->rpc, &ep);
	if (rc) {
		D_GOTO(err, ret = EIO);
	}
	IOF_TRACE_INFO(request, "Sending RPC to rank %d",
		       request->rpc->cr_ep.ep_rank);

	crt_req_addref(request->rpc);
	rc = crt_req_send(request->rpc, generic_cb, request);
	if (rc) {
		D_GOTO(err, ret = EIO);
	}
	return 0;
err:
	IOF_TRACE_ERROR(request, "Could not send rpc, rc = %d", ret);

	return ret;
}

static bool
ih_key_cmp(struct d_hash_table *htable, d_list_t *rlink,
	   const void *key, unsigned int ksize)
{
	const struct dfuse_inode_entry	*ie;
	const ino_t			*ino = key;

	ie = container_of(rlink, struct dfuse_inode_entry, ie_htl);

	return *ino == ie->stat.st_ino;
}

static void
ih_addref(struct d_hash_table *htable, d_list_t *rlink)
{
	struct dfuse_inode_entry	*ie;
	int				oldref;

	ie = container_of(rlink, struct dfuse_inode_entry, ie_htl);
	oldref = atomic_fetch_add(&ie->ie_ref, 1);
	IOF_TRACE_DEBUG(ie, "addref to %u", oldref + 1);
}

static bool
ih_decref(struct d_hash_table *htable, d_list_t *rlink)
{
	struct dfuse_inode_entry	*ie;
	int				oldref;

	ie = container_of(rlink, struct dfuse_inode_entry, ie_htl);
	oldref = atomic_fetch_sub(&ie->ie_ref, 1);
	IOF_TRACE_DEBUG(ie, "decref to %u", oldref - 1);
	return oldref == 1;
}

static void
ih_free(struct d_hash_table *htable, d_list_t *rlink)
{
	struct dfuse_projection_info	*fs_handle = htable->ht_priv;
	struct dfuse_inode_entry		*ie;

	ie = container_of(rlink, struct dfuse_inode_entry, ie_htl);

	IOF_TRACE_DEBUG(ie, "parent %lu", ie->parent);
	ie_close(fs_handle, ie);
	D_FREE(ie);
}

d_hash_table_ops_t hops = {.hop_key_cmp = ih_key_cmp,
			   .hop_rec_addref = ih_addref,
			   .hop_rec_decref = ih_decref,
			   .hop_rec_free = ih_free,
};

static void
dh_init(void *arg, void *handle)
{
	struct dfuse_dir_handle *dh = arg;

	IOC_REQUEST_INIT(&dh->open_req, handle);
	IOC_REQUEST_INIT(&dh->close_req, handle);
	dh->rpc = NULL;
}

/* Reset a RPC in a re-usable descriptor.  If the RPC pointer is valid
 * then drop the two references and zero the pointer.
 */
#define CHECK_AND_RESET_RPC(HANDLE, RPC)			\
	do {							\
		if ((HANDLE)->RPC) {				\
			crt_req_decref((HANDLE)->RPC);		\
			crt_req_decref((HANDLE)->RPC);		\
			(HANDLE)->RPC = NULL;			\
		}						\
	} while (0)

/* As CHECK_AND_RESET_RPC but take a dfuse_request as the second option
 * and work on the RPC in the request
 */
#define CHECK_AND_RESET_RRPC(HANDLE, REQUEST)			\
	CHECK_AND_RESET_RPC(HANDLE, REQUEST.rpc)

static bool
dh_reset(void *arg)
{
	struct dfuse_dir_handle	*dh = arg;
	int			rc;

	dh->reply_count = 0;

	/* If there has been an error on the local handle, or readdir() is not
	 * exhausted then ensure that all resources are freed correctly
	 */
	if (dh->rpc)
		crt_req_decref(dh->rpc);
	dh->rpc = NULL;

	if (dh->open_req.rpc)
		crt_req_decref(dh->open_req.rpc);

	if (dh->close_req.rpc)
		crt_req_decref(dh->close_req.rpc);

	rc = crt_req_create(dh->open_req.fsh->proj.crt_ctx, NULL,
			    FS_TO_OP(dh->open_req.fsh, opendir),
			    &dh->open_req.rpc);
	if (rc || !dh->open_req.rpc)
		return false;

	rc = crt_req_create(dh->open_req.fsh->proj.crt_ctx, NULL,
			    FS_TO_OP(dh->open_req.fsh, closedir),
			    &dh->close_req.rpc);
	if (rc || !dh->close_req.rpc) {
		crt_req_decref(dh->open_req.rpc);
		return false;
	}

	IOC_REQUEST_RESET(&dh->open_req);
	IOC_REQUEST_RESET(&dh->close_req);

	dh->open_req.ir_ht = RHS_INODE_NUM;
	dh->close_req.ir_ht = RHS_DIR;
	dh->close_req.ir_dir = dh;

	return true;
}

static void
dh_release(void *arg)
{
	struct dfuse_dir_handle *dh = arg;

	crt_req_decref(dh->open_req.rpc);
	crt_req_decref(dh->close_req.rpc);
}

/* Create a getattr descriptor for use with mempool.
 *
 * Two pools of descriptors are used here, one for getattr and a second
 * for getfattr.  The only difference is the RPC id so the datatypes are
 * the same, as are the init and release functions.
 */
static void
fh_init(void *arg, void *handle)
{
	struct dfuse_file_handle *fh = arg;

	IOC_REQUEST_INIT(&fh->open_req, handle);
	IOC_REQUEST_INIT(&fh->creat_req, handle);
	IOC_REQUEST_INIT(&fh->release_req, handle);
	fh->ie = NULL;
}

static bool
fh_reset(void *arg)
{
	struct dfuse_file_handle	*fh = arg;
	int			rc;

	IOC_REQUEST_RESET(&fh->open_req);
	CHECK_AND_RESET_RRPC(fh, open_req);

	fh->open_req.ir_ht = RHS_INODE_NUM;

	IOC_REQUEST_RESET(&fh->creat_req);
	CHECK_AND_RESET_RRPC(fh, creat_req);

	fh->creat_req.ir_ht = RHS_INODE_NUM;

	IOC_REQUEST_RESET(&fh->release_req);
	CHECK_AND_RESET_RRPC(fh, release_req);

	fh->release_req.ir_ht = RHS_FILE;
	fh->release_req.ir_file = fh;

	/* Used by creat but not open */
	fh->common.ep = fh->open_req.fsh->proj.grp->psr_ep;

	if (!fh->ie) {
		D_ALLOC_PTR(fh->ie);
		if (!fh->ie)
			return false;
		atomic_fetch_add(&fh->ie->ie_ref, 1);
	}

	rc = crt_req_create(fh->open_req.fsh->proj.crt_ctx, NULL,
			    FS_TO_OP(fh->open_req.fsh, open), &fh->open_req.rpc);
	if (rc || !fh->open_req.rpc) {
		D_FREE(fh->ie);
		return false;
	}

	rc = crt_req_create(fh->open_req.fsh->proj.crt_ctx, NULL,
			    FS_TO_OP(fh->open_req.fsh, create), &fh->creat_req.rpc);
	if (rc || !fh->creat_req.rpc) {
		D_FREE(fh->ie);
		crt_req_decref(fh->open_req.rpc);
		return false;
	}

	rc = crt_req_create(fh->open_req.fsh->proj.crt_ctx, NULL,
			    FS_TO_OP(fh->open_req.fsh, close), &fh->release_req.rpc);
	if (rc || !fh->release_req.rpc) {
		D_FREE(fh->ie);
		crt_req_decref(fh->open_req.rpc);
		crt_req_decref(fh->creat_req.rpc);
		return false;
	}

	crt_req_addref(fh->open_req.rpc);
	crt_req_addref(fh->creat_req.rpc);
	crt_req_addref(fh->release_req.rpc);
	return true;
}

static void
fh_release(void *arg)
{
	struct dfuse_file_handle *fh = arg;

	crt_req_decref(fh->open_req.rpc);
	crt_req_decref(fh->open_req.rpc);
	crt_req_decref(fh->creat_req.rpc);
	crt_req_decref(fh->creat_req.rpc);
	crt_req_decref(fh->release_req.rpc);
	crt_req_decref(fh->release_req.rpc);
	D_FREE(fh->ie);
}

#define COMMON_INIT(type)						\
	static void type##_common_init(void *arg, void *handle)		\
	{								\
		struct common_req *req = arg;				\
		IOC_REQUEST_INIT(&req->request, handle);		\
		req->opcode = FS_TO_OP(req->request.fsh, type);		\
	}
COMMON_INIT(getattr);
COMMON_INIT(setattr);
COMMON_INIT(close);

/* Reset and prepare for use a common descriptor */
static bool
common_reset(void *arg)
{
	struct common_req *req = arg;
	int rc;

	req->request.req = NULL;

	IOC_REQUEST_RESET(&req->request);
	CHECK_AND_RESET_RRPC(req, request);

	rc = crt_req_create(req->request.fsh->proj.crt_ctx, NULL,
			    req->opcode, &req->request.rpc);
	if (rc || !req->request.rpc) {
		IOF_TRACE_ERROR(req, "Could not create request, rc = %d", rc);
		return false;
	}
	crt_req_addref(req->request.rpc);

	return true;
}

/* Destroy a descriptor which could be either getattr or close */
static void
common_release(void *arg)
{
	struct common_req *req = arg;

	crt_req_decref(req->request.rpc);
	crt_req_decref(req->request.rpc);
}

#define ENTRY_INIT(type)						\
	static void type##_entry_init(void *arg, void *handle)		\
	{								\
		struct entry_req *req = arg;				\
		IOC_REQUEST_INIT(&req->request, handle);		\
		req->opcode = FS_TO_OP(req->request.fsh, type);		\
		req->dest = NULL;					\
		req->ie = NULL;						\
	}
ENTRY_INIT(lookup);
ENTRY_INIT(mkdir);
ENTRY_INIT(symlink);

static bool
entry_reset(void *arg)
{
	struct entry_req	*req = arg;
	int			rc;

	/* If this descriptor has previously been used then destroy the
	 * existing RPC
	 */
	IOC_REQUEST_RESET(&req->request);
	CHECK_AND_RESET_RRPC(req, request);

	req->request.ir_ht = RHS_INODE_NUM;
	/* Free any destination string on this descriptor.  This is only used
	 * for symlink to store the link target whilst the RPC is being sent
	 */
	D_FREE(req->dest);

	if (!req->ie) {
		D_ALLOC_PTR(req->ie);
		if (!req->ie)
			return false;
		atomic_fetch_add(&req->ie->ie_ref, 1);
	}

	/* Create a new RPC ready for later use.  Take an initial reference
	 * to the RPC so that it is not cleaned up after a successful send.
	 *
	 * After calling send the lookup code will re-take the dropped
	 * reference which means that on all subsequent calls to reset()
	 * or release() the ref count will be two.
	 *
	 * This means that both descriptor creation and destruction are
	 * done off the critical path.
	 */
	rc = crt_req_create(req->request.fsh->proj.crt_ctx, NULL, req->opcode,
			    &req->request.rpc);
	if (rc || !req->request.rpc) {
		IOF_TRACE_ERROR(req, "Could not create request, rc = %d", rc);
		D_FREE(req->ie);
		return false;
	}
	crt_req_addref(req->request.rpc);

	return true;
}

/* Destroy a descriptor which could be either getattr or getfattr */
static void
entry_release(void *arg)
{
	struct entry_req *req = arg;

	crt_req_decref(req->request.rpc);
	crt_req_decref(req->request.rpc);
	D_FREE(req->ie);
}

static void
rb_page_init(void *arg, void *handle)
{
	struct dfuse_rb *rb = arg;

	IOC_REQUEST_INIT(&rb->rb_req, handle);
	rb->buf_size = 4096;
	rb->fbuf.count = 1;
	rb->fbuf.buf[0].fd = -1;
	rb->failure = false;
	rb->lb.buf = NULL;
}

static void
rb_large_init(void *arg, void *handle)
{
	struct dfuse_rb *rb = arg;

	rb_page_init(arg, handle);
	rb->buf_size = rb->rb_req.fsh->max_read;
}

static bool
rb_reset(void *arg)
{
	struct dfuse_rb	*rb = arg;
	int		rc;

	IOC_REQUEST_RESET(&rb->rb_req);
	CHECK_AND_RESET_RRPC(rb, rb_req);

	rb->rb_req.ir_ht = RHS_FILE;

	if (rb->failure) {
		IOF_BULK_FREE(rb, lb);
		rb->failure = false;
	}

	if (!rb->lb.buf) {
		IOF_BULK_ALLOC(rb->rb_req.fsh->proj.crt_ctx, rb, lb,
			       rb->buf_size, false);
		if (!rb->lb.buf)
			return false;
	}

	rc = crt_req_create(rb->rb_req.fsh->proj.crt_ctx, NULL,
			    FS_TO_IOOP(rb->rb_req.fsh, 0), &rb->rb_req.rpc);
	if (rc || !rb->rb_req.rpc) {
		IOF_TRACE_ERROR(rb, "Could not create request, rc = %d", rc);
		IOF_BULK_FREE(rb, lb);
		return false;
	}
	crt_req_addref(rb->rb_req.rpc);

	return true;
}

static void
rb_release(void *arg)
{
	struct dfuse_rb *rb = arg;

	IOF_BULK_FREE(rb, lb);

	crt_req_decref(rb->rb_req.rpc);
	crt_req_decref(rb->rb_req.rpc);
}

static void
wb_init(void *arg, void *handle)
{
	struct dfuse_wb *wb = arg;

	IOC_REQUEST_INIT(&wb->wb_req, handle);
	wb->failure = false;
	wb->lb.buf = NULL;
}

static bool
wb_reset(void *arg)
{
	struct dfuse_wb	*wb = arg;
	int		rc;

	IOC_REQUEST_RESET(&wb->wb_req);
	CHECK_AND_RESET_RRPC(wb, wb_req);

	wb->wb_req.ir_ht = RHS_FILE;

	if (wb->failure) {
		IOF_BULK_FREE(wb, lb);
		wb->failure = false;
	}

	if (!wb->lb.buf) {
		IOF_BULK_ALLOC(wb->wb_req.fsh->proj.crt_ctx, wb, lb,
			       wb->wb_req.fsh->proj.max_write, true);
		if (!wb->lb.buf)
			return false;
	}

	rc = crt_req_create(wb->wb_req.fsh->proj.crt_ctx, NULL,
			    FS_TO_IOOP(wb->wb_req.fsh, 1), &wb->wb_req.rpc);
	if (rc || !wb->wb_req.rpc) {
		IOF_TRACE_ERROR(wb, "Could not create request, rc = %d", rc);
		IOF_BULK_FREE(wb, lb);
		return false;
	}
	crt_req_addref(wb->wb_req.rpc);

	return true;
}

static void
wb_release(void *arg)
{
	struct dfuse_wb *wb = arg;

	crt_req_decref(wb->wb_req.rpc);
	crt_req_decref(wb->wb_req.rpc);

	IOF_BULK_FREE(wb, lb);
}

/* Call crt_progress() on a context until it returns timeout
 * or an error.
 *
 * Returns -DER_SUCCESS on timeout or passes through any other errors.
 */
static int
dfuse_progress_drain(struct dfuse_ctx *dfuse_ctx)
{
	int ctx_rc;

	if (!dfuse_ctx->crt_ctx) {
		IOF_TRACE_WARNING(dfuse_ctx, "Null context");
		return -DER_SUCCESS;
	}

	do {
		ctx_rc = crt_progress(dfuse_ctx->crt_ctx, 1000000, NULL, NULL);

		if (ctx_rc != -DER_TIMEDOUT && ctx_rc != -DER_SUCCESS) {
			IOF_TRACE_WARNING(dfuse_ctx, "progress returned %d",
					  ctx_rc);
			return ctx_rc;
		}

	} while (ctx_rc != -DER_TIMEDOUT);
	return -DER_SUCCESS;
}

static void *
dfuse_thread(void *arg)
{
	struct dfuse_ctx	*dfuse_ctx = arg;
	int		rc;

	dfuse_tracker_signal(&dfuse_ctx->thread_start_tracker);
	do {
		rc = crt_progress(dfuse_ctx->crt_ctx,
				  dfuse_ctx->poll_interval,
				  dfuse_ctx->callback_fn,
				  &dfuse_ctx->thread_stop_tracker);

		if (rc == -DER_TIMEDOUT) {
			rc = 0;
			sched_yield();
		}

		if (rc != 0)
			IOF_TRACE_ERROR(dfuse_ctx, "crt_progress failed rc: %d",
					rc);

	} while (!dfuse_tracker_test(&dfuse_ctx->thread_stop_tracker));

	if (rc != 0)
		IOF_TRACE_ERROR(dfuse_ctx, "crt_progress error on shutdown "
				"rc: %d", rc);

	return (void *)(uintptr_t)rc;
}

/* Start a progress thread, return true on success */
static bool
dfuse_thread_start(struct dfuse_ctx *dfuse_ctx)
{
	int rc;

	dfuse_tracker_init(&dfuse_ctx->thread_start_tracker, 1);
	dfuse_tracker_init(&dfuse_ctx->thread_stop_tracker, 1);

	rc = pthread_create(&dfuse_ctx->thread, NULL,
			    dfuse_thread, dfuse_ctx);

	if (rc != 0) {
		IOF_TRACE_ERROR(dfuse_ctx, "Could not start progress thread");
		return false;
	}

	rc = pthread_setname_np(dfuse_ctx->thread, "IOF thread");
	if (rc != 0)
		IOF_TRACE_ERROR(dfuse_ctx, "Could not set thread name");

	dfuse_tracker_wait(&dfuse_ctx->thread_start_tracker);
	return true;
}

/* Stop the progress thread, and destroy the cart context
 *
 * Returns the return code of crt_context_destroy()
 */
static int
dfuse_thread_stop(struct dfuse_ctx *dfuse_ctx)
{
	void *rtn;

	if (!dfuse_ctx->thread)
		return 0;

	IOF_TRACE_INFO(dfuse_ctx, "Stopping CRT thread");
	dfuse_tracker_signal(&dfuse_ctx->thread_stop_tracker);
	pthread_join(dfuse_ctx->thread, &rtn);
	IOF_TRACE_INFO(dfuse_ctx,
		       "CRT thread stopped with %d",
		       (int)(uintptr_t)rtn);

	dfuse_ctx->thread = 0;

	return (int)(uintptr_t)rtn;
}

void
dfuse_reg(struct dfuse_state *dfuse_state, struct cnss_info *cnss_info)
{
	struct dfuse_group_info *group;
	int ret;

	dfuse_state->cnss_info = cnss_info;
	ret = crt_context_create(&dfuse_state->dfuse_ctx.crt_ctx);
	if (ret != -DER_SUCCESS) {
		IOF_TRACE_ERROR(dfuse_state, "Context not created");
		return;
	}

	IOF_TRACE_UP(&dfuse_state->dfuse_ctx, dfuse_state, "dfuse_ctx");

	ret = crt_context_set_timeout(dfuse_state->dfuse_ctx.crt_ctx, 7);
	if (ret != -DER_SUCCESS) {
		IOF_TRACE_ERROR(dfuse_state, "Context timeout not set");
		return;
	}

	if (!dfuse_thread_start(&dfuse_state->dfuse_ctx)) {
		IOF_TRACE_ERROR(dfuse_state, "Failed to create progress thread");
		return;
	}

	/* Despite the hard coding above, now we can do attaches in a loop */
	group = &dfuse_state->group;

	ret = dfuse_client_register(&group->grp.psr_ep,
				  &dfuse_state->proto,
				  &dfuse_state->io_proto);
	if (ret) {
		IOF_TRACE_ERROR(dfuse_state,
				"RPC registration failed with ret: %d", ret);
	}
}

static bool
initialize_projection(struct dfuse_state *dfuse_state,
		      struct dfuse_group_info *group)
{
	struct dfuse_projection_info	*fs_handle;
	struct fuse_args		args = {0};
	int				ret;
	struct fuse_lowlevel_ops	*fuse_ops = NULL;
	int				i;

	struct dfuse_pool_reg pt = {.init = dh_init,
				  .reset = dh_reset,
				  .release = dh_release,
				  POOL_TYPE_INIT(dfuse_dir_handle, dh_free_list)};

	struct dfuse_pool_reg fh = {.init = fh_init,
				  .reset = fh_reset,
				  .release = fh_release,
				  POOL_TYPE_INIT(dfuse_file_handle, fh_free_list)};

	struct dfuse_pool_reg common_t = {.reset = common_reset,
					.release = common_release,
					POOL_TYPE_INIT(common_req, list)};

	struct dfuse_pool_reg entry_t = {.reset = entry_reset,
				       .release = entry_release,
				       POOL_TYPE_INIT(entry_req, list)};

	struct dfuse_pool_reg rb_page = {.init = rb_page_init,
				       .reset = rb_reset,
				       .release = rb_release,
				       POOL_TYPE_INIT(dfuse_rb, rb_req.ir_list)};

	struct dfuse_pool_reg rb_large = {.init = rb_large_init,
					.reset = rb_reset,
					.release = rb_release,
					POOL_TYPE_INIT(dfuse_rb, rb_req.ir_list)};

	struct dfuse_pool_reg wb = {.init = wb_init,
				  .reset = wb_reset,
				  .release = wb_release,
				  POOL_TYPE_INIT(dfuse_wb, wb_req.ir_list)};

	D_ALLOC_PTR(fs_handle);
	if (!fs_handle)
		return false;

	IOF_TRACE_UP(fs_handle, dfuse_state, "dfuse_projection");

	if (fs_handle->ctx_num == 0) {
		fs_handle->ctx_num = 1;
	}

	D_ALLOC_ARRAY(fs_handle->ctx_array, fs_handle->ctx_num);
	if (!fs_handle->ctx_array) {
		IOF_TRACE_DOWN(fs_handle);
		D_FREE(fs_handle);
		return false;
	}

	for (i = 0; i < fs_handle->ctx_num; i++) {
		IOF_TRACE_UP(&fs_handle->ctx_array[i], fs_handle, "dfuse_ctx");
	}

	ret = dfuse_pool_init(&fs_handle->pool, fs_handle);
	if (ret != -DER_SUCCESS)
		D_GOTO(err, 0);

	fs_handle->dfuse_state = dfuse_state;
	fs_handle->proj.io_proto = dfuse_state->io_proto;

	IOF_TRACE_INFO(fs_handle, "%d cart threads",
		       fs_handle->ctx_num);

	ret = d_hash_table_create_inplace(D_HASH_FT_RWLOCK |
					  D_HASH_FT_EPHEMERAL,
					  3,
					  fs_handle, &hops,
					  &fs_handle->inode_ht);
	if (ret != 0)
		D_GOTO(err, 0);

	ret = D_MUTEX_INIT(&fs_handle->gah_lock, NULL);
	if (ret != 0)
		D_GOTO(err, 0);

	fs_handle->proj.progress_thread = 1;

	fs_handle->proj.grp = &group->grp;

	ret = crt_context_create(&fs_handle->proj.crt_ctx);
	if (ret) {
		IOF_TRACE_ERROR(fs_handle, "Could not create context");
		D_GOTO(err, 0);
	}

	for (i = 0; i < fs_handle->ctx_num; i++) {
		fs_handle->ctx_array[i].crt_ctx       = fs_handle->proj.crt_ctx;
		fs_handle->ctx_array[i].poll_interval = dfuse_state->dfuse_ctx.poll_interval;
		fs_handle->ctx_array[i].callback_fn   = dfuse_state->dfuse_ctx.callback_fn;

		/* TODO: Much better error checking is required here, not least
		 * terminating the thread if there are any failures in the rest
		 * of this function
		 */
		if (!dfuse_thread_start(&fs_handle->ctx_array[i])) {
			IOF_TRACE_ERROR(fs_handle, "Could not create thread");
			D_GOTO(err, 0);
		}
	}

	args.argc = 4;

	args.allocated = 1;
	D_ALLOC_ARRAY(args.argv, args.argc);
	if (!args.argv)
		D_GOTO(err, 0);

	D_STRNDUP(args.argv[0], "", 1);
	if (!args.argv[0])
		D_GOTO(err, 0);

	D_STRNDUP(args.argv[1], "-ofsname=dfuse", 32);
	if (!args.argv[1])
		D_GOTO(err, 0);

	D_STRNDUP(args.argv[2], "-osubtype=pam", 32);
	if (!args.argv[2])
		D_GOTO(err, 0);

	D_ASPRINTF(args.argv[3], "-omax_read=%u", fs_handle->max_read);
	if (!args.argv[3])
		D_GOTO(err, 0);

	fuse_ops = dfuse_get_fuse_ops(fs_handle->flags);
	if (!fuse_ops)
		D_GOTO(err, 0);

	/* Register the directory handle type
	 *
	 * This is done late on in the registration as the dh_int() and
	 * dh_reset() functions require access to fs_handle.
	 */
	fs_handle->dh_pool = dfuse_pool_register(&fs_handle->pool, &pt);
	if (!fs_handle->dh_pool)
		D_GOTO(err, 0);

	common_t.init = getattr_common_init;
	fs_handle->fgh_pool = dfuse_pool_register(&fs_handle->pool, &common_t);
	if (!fs_handle->fgh_pool)
		D_GOTO(err, 0);

	common_t.init = setattr_common_init;
	fs_handle->fsh_pool = dfuse_pool_register(&fs_handle->pool, &common_t);
	if (!fs_handle->fsh_pool)
		D_GOTO(err, 0);

	common_t.init = close_common_init;
	fs_handle->close_pool = dfuse_pool_register(&fs_handle->pool, &common_t);
	if (!fs_handle->close_pool)
		D_GOTO(err, 0);

	entry_t.init = lookup_entry_init;
	fs_handle->lookup_pool = dfuse_pool_register(&fs_handle->pool, &entry_t);
	if (!fs_handle->lookup_pool)
		D_GOTO(err, 0);

	entry_t.init = mkdir_entry_init;
	fs_handle->mkdir_pool = dfuse_pool_register(&fs_handle->pool, &entry_t);
	if (!fs_handle->mkdir_pool)
		D_GOTO(err, 0);

	entry_t.init = symlink_entry_init;
	fs_handle->symlink_pool = dfuse_pool_register(&fs_handle->pool, &entry_t);
	if (!fs_handle->symlink_pool)
		D_GOTO(err, 0);

	fs_handle->fh_pool = dfuse_pool_register(&fs_handle->pool, &fh);
	if (!fs_handle->fh_pool)
		D_GOTO(err, 0);

	fs_handle->rb_pool_page = dfuse_pool_register(&fs_handle->pool, &rb_page);
	if (!fs_handle->rb_pool_page)
		D_GOTO(err, 0);

	fs_handle->rb_pool_large = dfuse_pool_register(&fs_handle->pool, &rb_large);
	if (!fs_handle->rb_pool_large)
		D_GOTO(err, 0);

	fs_handle->write_pool = dfuse_pool_register(&fs_handle->pool, &wb);
	if (!fs_handle->write_pool)
		D_GOTO(err, 0);

	if (!cnss_register_fuse(fs_handle->dfuse_state->cnss_info,
				fuse_ops,
				&args,
				fs_handle->mnt_dir.name,
				(fs_handle->flags & IOF_CNSS_MT) != 0,
				fs_handle,
				&fs_handle->session)) {
		IOF_TRACE_ERROR(fs_handle, "Unable to register FUSE fs");
		D_GOTO(err, 0);
	}

	D_FREE(fuse_ops);

	IOF_TRACE_DEBUG(fs_handle, "Fuse mount installed at: '%s'",
			fs_handle->mnt_dir.name);

	return true;
err:
	dfuse_pool_destroy(&fs_handle->pool);
	D_FREE(fuse_ops);
	D_FREE(fs_handle);
	return false;
}

void
dfuse_post_start(struct dfuse_state *dfuse_state)
{
	struct dfuse_group_info	*group = &dfuse_state->group;

	initialize_projection(dfuse_state, group);
}

static int
ino_flush(d_list_t *rlink, void *arg)
{
	struct dfuse_projection_info *fs_handle = arg;
	struct dfuse_inode_entry *ie = container_of(rlink,
						  struct dfuse_inode_entry,
						  ie_htl);
	int rc;

	/* Only evict entries that are direct children of the root, the kernel
	 * will walk the tree for us
	 */
	if (ie->parent != 1)
		return 0;

	rc = fuse_lowlevel_notify_inval_entry(fs_handle->session,
					      ie->parent,
					      ie->name,
					      strlen(ie->name));
	if (rc != 0)
		IOF_TRACE_WARNING(ie,
				  "%lu %lu '%s': %d %s",
				  ie->parent, ie->stat.st_ino, ie->name, rc,
				  strerror(-rc));
	else
		IOF_TRACE_INFO(ie,
			       "%lu %lu '%s': %d %s",
			       ie->parent, ie->stat.st_ino, ie->name, rc,
			       strerror(-rc));

	/* If the FUSE connection is dead then do not traverse further, it doesn't
	 * matter what gets returned here, as long as it's negative
	 */
	if (rc == -EBADF)
		return -DER_NO_HDL;

	return -DER_SUCCESS;
}

/* Called once per projection, before the FUSE filesystem has been torn down */
void
dfuse_flush_fuse(struct dfuse_projection_info *fs_handle)
{
	int rc;

	IOF_TRACE_INFO(fs_handle, "Flushing inode table");

	rc = d_hash_table_traverse(&fs_handle->inode_ht, ino_flush,
				   fs_handle);

	IOF_TRACE_INFO(fs_handle, "Flush complete: %d", rc);
}

/* Called once per projection, after the FUSE filesystem has been torn down */
int
dfuse_deregister_fuse(struct dfuse_projection_info *fs_handle)
{
	d_list_t	*rlink;
	uint64_t	refs = 0;
	int		handles = 0;
	int		rc;
	int		rcp = 0;
	int		i;

	IOF_TRACE_INFO(fs_handle, "Draining inode table");
	do {
		struct dfuse_inode_entry *ie;
		uint ref;

		rlink = d_hash_rec_first(&fs_handle->inode_ht);

		if (!rlink)
			break;

		ie = container_of(rlink, struct dfuse_inode_entry, ie_htl);

		ref = atomic_load_consume(&ie->ie_ref);

		IOF_TRACE_DEBUG(ie, "Dropping %d", ref);

		refs += ref;
		ie->parent = 0;
		d_hash_rec_ndecref(&fs_handle->inode_ht, ref, rlink);
		handles++;
	} while (rlink);

	if (handles) {
		IOF_TRACE_WARNING(fs_handle,
				  "dropped %lu refs on %u inodes",
				  refs, handles);
	} else {
		IOF_TRACE_INFO(fs_handle,
			       "dropped %lu refs on %u inodes",
			       refs, handles);
	}

	rc = d_hash_table_destroy_inplace(&fs_handle->inode_ht, false);
	if (rc) {
		IOF_TRACE_WARNING(fs_handle, "Failed to close inode handles");
		rcp = EINVAL;
	}

	/* Stop the progress thread for this projection and delete the context
	 */

	for (i = 0; i < fs_handle->ctx_num; i++) {
		rc = dfuse_thread_stop(&fs_handle->ctx_array[i]);
		if (rc != 0)
			IOF_TRACE_ERROR(fs_handle,
					"thread[%d] stop returned %d", i, rc);
	}

	do {
		/* If this context has a pool associated with it then reap
		 * any descriptors with it so there are no pending RPCs when
		 * we call context_destroy.
		 */
		bool active;

		do {
			rc = dfuse_progress_drain(&fs_handle->ctx_array[0]);

			active = dfuse_pool_reclaim(&fs_handle->pool);

			if (!active)
				break;

			IOF_TRACE_INFO(fs_handle,
				       "Active descriptors, waiting for one second");

		} while (active && rc == -DER_SUCCESS);

		rc = crt_context_destroy(fs_handle->proj.crt_ctx, false);
		if (rc == -DER_BUSY)
			IOF_TRACE_INFO(fs_handle, "RPCs in flight, waiting");
		else if (rc != DER_SUCCESS)
			IOF_TRACE_ERROR(fs_handle,
					"Could not destroy context %d",
					rc);
	} while (rc == -DER_BUSY);

	if (rc != -DER_SUCCESS)
		IOF_TRACE_ERROR(fs_handle, "Count not destroy context");

	dfuse_pool_destroy(&fs_handle->pool);

	rc = pthread_mutex_destroy(&fs_handle->gah_lock);
	if (rc != 0) {
		IOF_TRACE_ERROR(fs_handle,
				"Failed to destroy lock %d %s",
				rc, strerror(rc));
		rcp = rc;
	}

	for (i = 0; i < fs_handle->ctx_num; i++) {
		IOF_TRACE_DOWN(&fs_handle->ctx_array[i]);
	}
	d_list_del_init(&fs_handle->link);

	D_FREE(fs_handle->ctx_array);

	return rcp;
}

void
dfuse_finish(struct dfuse_state *dfuse_state)
{
	int rc;

	/* Stop progress thread */
	rc = dfuse_thread_stop(&dfuse_state->dfuse_ctx);
	if (rc != 0)
		IOF_TRACE_ERROR(dfuse_state,
				"thread stop returned %d", rc);

	if (dfuse_state->dfuse_ctx.crt_ctx) {

		rc = dfuse_progress_drain(&dfuse_state->dfuse_ctx);
		if (rc != 0)
			IOF_TRACE_ERROR(dfuse_state,
					"could not drain context %d", rc);

		rc = crt_context_destroy(dfuse_state->dfuse_ctx.crt_ctx, false);
		if (rc != -DER_SUCCESS)
			IOF_TRACE_ERROR(dfuse_state, "Could not destroy context %d",
					rc);
		IOF_TRACE_DOWN(&dfuse_state->dfuse_ctx);
	}

	IOF_TRACE_DOWN(dfuse_state);
	D_FREE(dfuse_state);
}

struct dfuse_state *
dfuse_plugin_init()
{
	struct dfuse_state *dfuse_state;

	D_ALLOC_PTR(dfuse_state);

	return dfuse_state;
}
