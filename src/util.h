#ifndef FATDEFRAG_UTIL_H
#define FATDEFRAG_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

uint16_t rd_le16(const void *p);
uint32_t rd_le32(const void *p);
uint64_t rd_le64(const void *p);
void wr_le16(void *p, uint16_t v);
void wr_le32(void *p, uint32_t v);
void wr_le64(void *p, uint64_t v);

int utf16le_to_utf8(const uint16_t *u, int n, char *out, size_t outsz);
int utf8_to_utf16le(const char *s, uint16_t *out, int maxn);

void path_normalize(const char *in, char *out, size_t n);
int path_equal(const char *a, const char *b);
int path_join(char *dst, size_t n, const char *prefix, const char *name);
const char *path_basename(const char *p);

void fat_83_to_name(const uint8_t name[11], char *out, size_t n);
int fat_name_valid_char(unsigned char c);
void fat_make_short_name(const char *longname, uint8_t out[11], int seq);

uint32_t count_extents(const uint32_t *cl, uint32_t n);

void warn_backup(void);

#endif
