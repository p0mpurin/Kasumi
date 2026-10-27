#include <mbedtls/entropy.h>
#include <mbedtls/entropy_poll.h>
#include <psp2/kernel/rng.h>

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    *olen = 0;
    /* sceKernelGetRandomNumber fills at most 64 bytes a call. */
    for (size_t done = 0; done < len;) {
        const size_t n = len - done < 64 ? len - done : 64;
        if (sceKernelGetRandomNumber(output + done, n) < 0) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        done += n;
    }
    *olen = len;
    return 0;
}
