#pragma once

#define APP_NAME "Kasumi"
#define APP_BUILD "71"
#define APP_DATA_DIR "sdmc:/3ds/kasumi"
/* The project was called OpenNOW-3DS before it was renamed to Kasumi. */
#define APP_LEGACY_DATA_DIR "sdmc:/3ds/opennow-3ds"

/* Create the data folder and move a saved login, device ID and settings
 * over from the legacy folder on first launch after the rename. */
void app_paths_migrate(void);
