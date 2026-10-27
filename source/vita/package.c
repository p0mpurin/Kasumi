#include "package.h"
#include "app_paths.h"

#include <psp2/io/devctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Kasumi on the Vita is a VPK. Installing one over the running app needs
 * the system's package promoter (unsafe homebrew), so the updater saves the
 * checked download next to Kasumi's data and the player installs it with
 * VitaShell. */
#define PACKAGE_PATH APP_DATA_DIR "/Kasumi.vpk"

void package_init(const char *self_path) { (void)self_path; }
const char *package_file(void) { return "Kasumi.vpk"; }
const char *package_kind(void) { return "vpk"; }
const char *package_label(void) { return "VPK"; }

uint64_t package_space_free(void)
{
    SceIoDevInfo info;
    memset(&info, 0, sizeof(info));
    if (sceIoDevctl("ux0:", 0x3001, NULL, 0, &info, sizeof(info)) < 0) return UINT64_MAX;
    return (uint64_t)info.free_size;
}

static PackageResult failed(char *error, size_t error_size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
static PackageResult failed(char *error, size_t error_size, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
    return PACKAGE_FAILED;
}

PackageResult package_install(const unsigned char *data, size_t size, void (*progress)(unsigned permille),
                              char *error, size_t error_size)
{
    mkdir(APP_DATA_DIR, 0777);
    FILE *f = fopen(PACKAGE_PATH ".part", "wb");
    if (!f) return failed(error, error_size, "Could not write %s", PACKAGE_PATH);
    const bool written = fwrite(data, 1, size, f) == size;
    fclose(f);
    if (!written) {
        remove(PACKAGE_PATH ".part");
        return failed(error, error_size, "Memory card write failed");
    }
    remove(PACKAGE_PATH);
    if (rename(PACKAGE_PATH ".part", PACKAGE_PATH) != 0) return failed(error, error_size, "Could not save %s", PACKAGE_PATH);
    progress(1000);
    /* Not installed yet: the message says where the VPK is. */
    return failed(error, error_size, "Saved %s: install it with VitaShell", PACKAGE_PATH);
}

bool package_can_relaunch(void) { return false; }
void package_relaunch(void) {}
