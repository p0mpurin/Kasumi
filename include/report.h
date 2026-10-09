#pragma once

#include <stdbool.h>

#include "app_paths.h"

/* Opt-in diagnostic reports: sent only when the player chooses Settings >
 * System > Send diagnostic report and confirms. The report holds the
 * diagnostic log of this run and the previous one (IP addresses shortened),
 * settings.json and the newest Luma crash dump from the last 3 days. The
 * service (server/report-worker) answers with a code the player shares. */

/* False when this build has no report service configured. */
bool report_available(void);

/* Worker thread only. `trigger` says why ("manual", "unclean-exit",
 * "launch-failed"...). On success the code is in report_code(); on failure
 * report_error() says why. */
bool report_send(const char *trigger);

/* Written at a normal exit; its absence from the previous run's log (same
 * version) means that run crashed, froze or lost power. */
#define REPORT_CLEAN_EXIT "exit clean"
bool report_previous_run_unclean(void);

/* The newest finished session's performance summary, waiting to be sent
 * (written by perf_stats; survives an exit). */
#define REPORT_STATS_PENDING_PATH APP_DATA_DIR "/stats-pending.json"
bool report_stats_pending(void);
/* A summary or launch record was queued (true) or the files were dropped
 * (false). */
void report_stats_mark_pending(bool pending);
/* Worker thread only: send and delete the pending summary. */
bool report_send_stats(void);

/* "K7F-2QX" after a successful send. */
const char *report_code(void);
const char *report_error(void);
