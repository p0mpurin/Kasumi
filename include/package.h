#pragma once

/* How this copy of Kasumi is installed, and replacing it with a newer one
 * (updater.c downloads and checks it). Each console has its own
 * (source/3ds/package.c: a CIA or a .3dsx; the Vita: a VPK). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* self_path is argv[0] (the .3dsx's path when homebrew). */
void package_init(const char *self_path);
/* The release file this copy updates from ("Kasumi.cia"). */
const char *package_file(void);
/* Its key in the dev server's answer ("cia"). */
const char *package_kind(void);
/* What Settings calls it ("CIA", ".3dsx"). */
const char *package_label(void);
/* Free space where it installs, in bytes; UINT64_MAX when unknown. */
uint64_t package_space_free(void);

typedef enum { PACKAGE_INSTALLED, PACKAGE_FAILED, PACKAGE_ALREADY_INSTALLED } PackageResult;
/* Installs a verified download. progress gets 0-1000 as it goes; on
 * failure error says why (the old version is untouched). */
PackageResult package_install(const unsigned char *data, size_t size, void (*progress)(unsigned permille),
                              char *error, size_t error_size);
/* After an install: whether Kasumi can start the new version itself on
 * exit, and asking it to. */
bool package_can_relaunch(void);
void package_relaunch(void);
