#ifndef BERRY_QR_DISPLAY_H
#define BERRY_QR_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int berry_qr_hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static inline bool berry_qr_valid(int size, const char *bits, int64_t expires, int64_t now) {
    if (!bits || size < 29 || size > 65 || (size - 25) % 4 || expires <= now || expires > now + 25)
        return false;
    size_t length = (size_t)2 * (((size_t)size * (size_t)size + 7) / 8);
    if (strlen(bits) != length) return false;
    for (size_t i = 0; i < length; ++i) if (berry_qr_hex(bits[i]) < 0) return false;
    return true;
}

static inline bool berry_qr_black(int size, const char *bits, int x, int y) {
    if (!bits || x < 0 || y < 0 || x >= size || y >= size) return false;
    size_t index = (size_t)y * (size_t)size + (size_t)x;
    int nibble = berry_qr_hex(bits[index / 4]);
    return nibble >= 0 && (nibble & (8 >> (index % 4))) != 0;
}

#endif
