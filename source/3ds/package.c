#include "package.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Homebrew Launcher: a .3dsx replaced in place. Installed: a CIA through
 * the system installer. */
static char g_self_path[256];
static bool g_installed_cia;

void package_init(const char *self_path)
{
    snprintf(g_self_path, sizeof(g_self_path), "%s", self_path ? self_path : "");
}

static bool homebrew(void) { return envIsHomebrew(); }

const char *package_file(void) { return homebrew() ? "Kasumi.3dsx" : "Kasumi.cia"; }
const char *package_kind(void) { return homebrew() ? "3dsx" : "cia"; }
const char *package_label(void) { return homebrew() ? ".3dsx" : "CIA"; }

uint64_t package_space_free(void)
{
    FS_ArchiveResource resource;
    if (R_FAILED(FSUSER_GetSdmcArchiveResource(&resource))) return UINT64_MAX;
    return (uint64_t)resource.freeClusters * resource.clusterSize;
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

/* AM's answer when the CIA's content is exactly what is installed already
 * (a dev build that was just put on with FBI). */
#define AM_ALREADY_INSTALLED ((Result)0xC8E083FC)

static PackageResult install_cia(const unsigned char *data, size_t size, void (*progress)(unsigned permille),
                                 char *error, size_t error_size)
{
    if (R_FAILED(amInit())) return failed(error, error_size, "Could not open the system installer (am)");
    Handle cia;
    Result rc = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
    if (R_FAILED(rc)) {
        amExit();
        return failed(error, error_size, "Installer refused to start (0x%08lX)", (unsigned long)rc);
    }
    const size_t chunk = 128 * 1024;
    for (size_t offset = 0; offset < size; offset += chunk) {
        const size_t n = size - offset < chunk ? size - offset : chunk;
        u32 written = 0;
        rc = FSFILE_Write(cia, &written, offset, data + offset, (u32)n, 0);
        if (R_FAILED(rc) || written != n) {
            AM_CancelCIAInstall(cia);
            amExit();
            if (rc == AM_ALREADY_INSTALLED) return PACKAGE_ALREADY_INSTALLED;
            return failed(error, error_size, "Install write failed (0x%08lX); the old version is untouched",
                          (unsigned long)rc);
        }
        progress((unsigned)((offset + n) * 1000 / size));
    }
    rc = AM_FinishCiaInstall(cia);
    amExit();
    if (R_FAILED(rc)) return failed(error, error_size, "Install could not finish (0x%08lX)", (unsigned long)rc);
    g_installed_cia = true;
    return PACKAGE_INSTALLED;
}

static PackageResult install_3dsx(const unsigned char *data, size_t size, void (*progress)(unsigned permille),
                                  char *error, size_t error_size)
{
    if (!g_self_path[0] || strncmp(g_self_path, "sdmc:/", 6))
        return failed(error, error_size, "Could not tell where Kasumi.3dsx is; update it by hand");
    char next[300], old[300];
    snprintf(next, sizeof(next), "%s.new", g_self_path);
    snprintf(old, sizeof(old), "%s.old", g_self_path);
    FILE *f = fopen(next, "wb");
    if (!f) return failed(error, error_size, "Could not write %s", next);
    const bool written = fwrite(data, 1, size, f) == size;
    fclose(f);
    if (!written) {
        remove(next);
        return failed(error, error_size, "SD card write failed");
    }
    progress(500);
    /* Keep the old file until the new one is in place. */
    remove(old);
    if (rename(g_self_path, old) != 0) {
        remove(next);
        return failed(error, error_size, "Could not replace Kasumi.3dsx");
    }
    if (rename(next, g_self_path) != 0) {
        rename(old, g_self_path);
        return failed(error, error_size, "Could not replace Kasumi.3dsx");
    }
    remove(old);
    progress(1000);
    return PACKAGE_INSTALLED;
}

PackageResult package_install(const unsigned char *data, size_t size, void (*progress)(unsigned permille),
                              char *error, size_t error_size)
{
    return homebrew() ? install_3dsx(data, size, progress, error, error_size)
                      : install_cia(data, size, progress, error, error_size);
}

bool package_can_relaunch(void) { return g_installed_cia; }

void package_relaunch(void)
{
    /* On exit, the system starts this title again: the new version. */
    if (g_installed_cia) aptSetChainloaderToSelf();
}
