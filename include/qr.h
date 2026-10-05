#pragma once

/* QR codes for short text, made on the console: byte mode, error
 * correction M, versions 1-6 (up to 106 bytes). The same encoder as
 * tools/make_qr.py. */

#define QR_MAX_SIZE 41

/* Fills modules (size x size, row by row, 1 = dark) and returns the size,
 * or 0 when the text is too long. */
int qr_encode(const char *text, unsigned char modules[QR_MAX_SIZE * QR_MAX_SIZE]);
