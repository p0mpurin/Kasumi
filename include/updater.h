#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Self-update from GitHub Releases (github.com/p0mpurin/Kasumi).
 *
 * Check: the releases list is read and the newest release for the chosen
 * channel (stable, or beta which includes pre-releases) is compared with
 * APP_VERSION. Install: the CIA (or, when run from the Homebrew Launcher,
 * the .3dsx) is downloaded to the SD card, verified against SHA256SUMS from
 * the same release, then installed: the CIA through the system installer,
 * which only commits once the whole title is written, the .3dsx by writing
 * a new file and swapping it in. Network steps run on the network worker. */

typedef enum {
    UPDATE_IDLE,
    UPDATE_CHECKING,
    UPDATE_UP_TO_DATE,
    UPDATE_AVAILABLE,
    UPDATE_DOWNLOADING,
    UPDATE_VERIFYING,
    UPDATE_INSTALLING,
    UPDATE_INSTALLED,
    UPDATE_FAILED
} UpdateState;

typedef struct {
    UpdateState state;
    /* The newer release, e.g. "0.9.1" or "0.9.1-beta.2". */
    char latest[32];
    char published[16];
    bool prerelease;
    char notes[1600];
    unsigned long size_bytes;
    /* 0..1000 for the current phase. */
    unsigned progress;
    char error[128];
    int64_t checked_at;
} UpdateInfo;

/* Reads update.json; self_path is argv[0] (the .3dsx path when homebrew). */
void updater_init(const char *self_path);
UpdateInfo updater_info(void);
/* True when installing replaces a .3dsx rather than the installed CIA. */
bool updater_is_3dsx(void);
/* Auto-check is due (at most every 12 hours). */
bool updater_check_due(void);

/* Network worker. */
bool updater_check(bool include_beta);
bool updater_install(void);

/* UI: after an install, restart into the new version (CIA only). */
bool updater_can_relaunch(void);
void updater_relaunch(void);
/* The first start of a new version: its release notes, once. */
bool updater_take_whats_new(char *version, unsigned version_size, char *notes, unsigned notes_size);
/* Hide the "update available" badge for this version until the next one. */
void updater_dismiss(void);
bool updater_dismissed(void);
