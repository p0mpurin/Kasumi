/* Kasumi: the configure results jansson needs on the PS Vita (newlib). */
#ifndef JANSSON_PRIVATE_CONFIG_H
#define JANSSON_PRIVATE_CONFIG_H

#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_STRTOLL 1
#define USE_URANDOM 0
#define USE_WINDOWS_CRYPTOAPI 0
#define INITIAL_HASHTABLE_ORDER 3

#endif
