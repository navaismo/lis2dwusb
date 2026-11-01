#ifndef LIS2DW_PROTOCOL_H
#define LIS2DW_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LIS_PROTOCOL_VERSION 2
#define LIS_LINE_SIZE 160
#define LIS_COUNTS_PER_G 16384

/* CRC-16/CCITT-FALSE covers the payload before the asterisk. */
static inline uint16_t lis_crc16(const char *data, size_t size) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= (uint16_t)(unsigned char)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}
static inline int lis_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static inline int lis_token_valid(const char *token) {
    if (strlen(token) != 8) return 0;
    for (unsigned i = 0; i < 8; ++i)
        if (lis_hex(token[i]) < 0) return 0;
    return 1;
}
static inline int lis_frame(char *output, size_t capacity, const char *payload) {
    int size = snprintf(output, capacity, "%s*%04X\n", payload,
                        (unsigned)lis_crc16(payload, strlen(payload)));
    return size >= 0 && (size_t)size < capacity ? size : -1;
}
/* The caller strips CR/LF and rejects embedded NUL bytes. */
static inline int lis_unframe(char *line) {
    char *suffix = strrchr(line, '*');
    if (!suffix || strlen(suffix + 1) != 4) return 0;
    unsigned expected = 0;
    for (unsigned i = 1; i <= 4; ++i) {
        int digit = lis_hex(suffix[i]);
        if (digit < 0) return 0;
        expected = (expected << 4) | (unsigned)digit;
    }
    if (lis_crc16(line, (size_t)(suffix - line)) != expected) return 0;
    *suffix = '\0';
    return 1;
}
static inline int lis_rate_valid(unsigned rate) {
    return rate == 200 || rate == 400 || rate == 800 || rate == 1600;
}
#endif
