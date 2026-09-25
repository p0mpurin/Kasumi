#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool h264_sps_dimensions(const uint8_t *annex_b, size_t size,
                         unsigned *width, unsigned *height,
                         unsigned *profile, unsigned *level, unsigned *refs,
                         uint32_t *signature);
