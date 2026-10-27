#pragma once

#define APP_NAME "Kasumi"
#define APP_BUILD "160"
/* Set by the Makefile from VERSION_MAJOR / MINOR / MICRO / SUFFIX. */
#ifndef APP_VERSION
#define APP_VERSION "0.0.0-dev"
#endif
/* GitHub repository that releases (and updates) come from. */
#define APP_REPOSITORY "p0mpurin/Kasumi"
/* Where Kasumi keeps its files, and the folder that holds that one. */
#if defined(__vita__)
#define APP_ROOT_DIR "ux0:data"
#define APP_DATA_DIR "ux0:data/kasumi"
#else
#define APP_ROOT_DIR "sdmc:/3ds"
#define APP_DATA_DIR "sdmc:/3ds/kasumi"
#endif
/* Opt-in diagnostic reports go here (server/report-worker). A URL containing
 * "CHANGE-ME" disables the feature (for forks without a service). */
#define REPORT_BASE "https://kasumi-reports.p0mpurin.workers.dev"
#define REPORT_URL REPORT_BASE "/report"
#define STATS_URL REPORT_BASE "/stats"
#define LAUNCHES_URL REPORT_BASE "/launches"
/* The project was called OpenNOW-3DS before it was renamed to Kasumi. */
#define APP_LEGACY_DATA_DIR APP_ROOT_DIR "/opennow-3ds"

/* Create the data folder and move a saved login, device ID and settings
 * over from the legacy folder on first launch after the rename. */
void app_paths_migrate(void);
