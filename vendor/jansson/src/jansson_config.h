/* Kasumi: jansson_config.h.in filled in for the PS Vita's toolchain (GCC,
 * newlib). The 3DS build uses devkitPro's portlibs jansson instead. */
#ifndef JANSSON_CONFIG_H
#define JANSSON_CONFIG_H

#ifdef __cplusplus
#define JSON_INLINE inline
#else
#define JSON_INLINE inline
#endif

#define JSON_INTEGER_IS_LONG_LONG 1
#define JSON_HAVE_LOCALECONV 0
#define JSON_HAVE_ATOMIC_BUILTINS 1
#define JSON_HAVE_SYNC_BUILTINS 1
#define JSON_PARSER_MAX_DEPTH 2048

#endif
