#pragma once

/* Background thread that owns every blocking NVIDIA call (sign-in, catalog,
 * CloudMatch create/poll/stop, NVST connect), so the UI and the stream never
 * wait on the network. The UI thread reads a published snapshot of the
 * client state and never mutates it directly. */

#include <stdbool.h>

#include "gfn_client.h"
#include "nvst_signal.h"

typedef enum {
    NET_JOB_NONE,
    NET_JOB_BEGIN_LOGIN,
    NET_JOB_CANCEL_LOGIN,
    NET_JOB_LOAD_LIBRARY,
    NET_JOB_SEARCH,
    NET_JOB_START_SESSION,
    NET_JOB_STOP_SESSION,
    NET_JOB_RESTART_SESSION,
    NET_JOB_START_SIGNAL,
    NET_JOB_SIGN_OUT,
    /* Show the saved library again (after a search): no network. */
    NET_JOB_LIBRARY_CACHED,
    NET_JOB_CONNECTION_TEST,
    /* Text "beta" includes pre-releases. */
    NET_JOB_UPDATE_CHECK,
    NET_JOB_UPDATE_INSTALL
} NetJobKind;

typedef struct {
    NetJobKind kind;
    bool ok;
    bool cancelled;
    unsigned serial;
} NetJobResult;

/* Takes a copy of the initialised client; `signal` is started by the worker
 * for NET_JOB_START_SIGNAL and otherwise belongs to the UI thread. */
bool net_worker_start(const GfnClient *client, NvstSignal *signal);
void net_worker_stop(void);
/* Queue one job; false while another job is pending or running. */
bool net_worker_submit(NetJobKind kind, const char *text, const GfnGame *game);
bool net_worker_busy(void);
NetJobKind net_worker_current_job(void);
/* Abort the HTTP request in flight for the current job. */
void net_worker_cancel(void);
/* True while the worker is connecting `signal`; the UI must not touch it. */
bool net_worker_signal_starting(void);
/* Copy the newest published client state into `out` if it changed. */
bool net_worker_sync(GfnClient *out);
NetJobResult net_worker_last_result(void);
/* Block until the current job finishes or the timeout passes. */
void net_worker_wait_idle(unsigned timeout_ms);
