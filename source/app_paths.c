#include "app_paths.h"

#include <stdio.h>
#include <sys/stat.h>

void app_paths_migrate(void)
{
    static const char *const files[] = { "gfn-session.json", "device-id.txt", "settings.json" };
    mkdir("sdmc:/3ds", 0777);
    mkdir(APP_DATA_DIR, 0777);
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        char from[96], to[96];
        struct stat info;
        snprintf(from, sizeof(from), "%s/%s", APP_LEGACY_DATA_DIR, files[i]);
        snprintf(to, sizeof(to), "%s/%s", APP_DATA_DIR, files[i]);
        if (stat(to, &info) == 0 || stat(from, &info) != 0) continue;
        rename(from, to);
    }
}
