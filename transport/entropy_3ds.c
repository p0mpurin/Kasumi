#include <3ds.h>
#include <mbedtls/entropy.h>
#include <mbedtls/entropy_poll.h>

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    *olen = 0;
    if (R_FAILED(psInit())) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    Result rc = PS_GenerateRandomBytes(output, len);
    psExit();
    if (R_FAILED(rc)) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = len;
    return 0;
}
