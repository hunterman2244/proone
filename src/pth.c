#include <errno.h>

#include "util_rt.h"
#include "pth.h"


void prne_init_worker (prne_worker_t *w) {
	w->ctx = NULL;
	w->entry = NULL;
	w->fin = NULL;
	w->free_ctx = NULL;
	w->pth = NULL;
}

void prne_free_worker (prne_worker_t *w) {
	if (w->ctx != NULL) {
		prne_assert(w->free_ctx != NULL);
		w->free_ctx(w->ctx);
		w->ctx = NULL;
	}
}

void prne_fin_worker (prne_worker_t *w) {
	if (w->fin != NULL) {
		w->fin(w->ctx);
	}
}

bool prne_pth_cv_notify (pth_mutex_t *lock, pth_cond_t *cond, bool broadcast) {
	bool ret;

	if (pth_mutex_acquire(lock, FALSE, NULL)) {
		ret = pth_cond_notify(cond, broadcast) != 0;
		prne_dbgtrap(pth_mutex_release(lock));
	}
	else {
		ret = false;
	}

	return ret;
}

pth_time_t prne_pth_tstimeout (const struct timespec ts) {
	return pth_timeout(ts.tv_sec, ts.tv_nsec / 1000);
}
