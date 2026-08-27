#include "util.h"
#include "../include/fatdefrag.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint16_t rd_le16(const void *p)
{
    const uint8_t *b = p;
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

uint32_t rd_le32(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint64_t rd_le64(const void *p)
{
    const uint8_t *b = p;
    return (uint64_t)rd_le32(b) | ((uint64_t)rd_le32(b + 4) << 32);
}

void wr_le16(void *p, uint16_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

void wr_le32(void *p, uint32_t v)
{
    uint8_t *b = p;
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

void wr_le64(void *p, uint64_t v)
{
    wr_le32(p, (uint32_t)v);
    wr_le32((uint8_t *)p + 4, (uint32_t)(v >> 32));
}

int utf16le_to_utf8(const uint16_t *u, int n, char *out, size_t outsz)
{
    size_t o = 0;
    if (outsz == 0)
        return -1;
    for (int i = 0; i < n; i++) {
        uint32_t cp = u[i];
        if (cp == 0 || cp == 0xFFFF)
            break;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
            uint32_t lo = u[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            if (o + 1 >= outsz)
                return -1;
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= outsz)
                return -1;
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 >= outsz)
                return -1;
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 >= outsz)
                return -1;
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    return (int)o;
}

int utf8_to_utf16le(const char *s, uint16_t *out, int maxn)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < maxn) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = (*p & 0x1F) << 6;
            cp |= (p[1] & 0x3F);
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = (*p & 0x0F) << 12;
            cp |= (p[1] & 0x3F) << 6;
            cp |= (p[2] & 0x3F);
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            cp = (*p & 0x07) << 18;
            cp |= (p[1] & 0x3F) << 12;
            cp |= (p[2] & 0x3F) << 6;
            cp |= (p[3] & 0x3F);
            p += 4;
        } else {
            cp = '?';
            p++;
        }
        if (cp >= 0x10000) {
            if (n + 1 >= maxn)
                break;
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    return n;
}

void path_normalize(const char *in, char *out, size_t n)
{
    size_t o = 0;
    while (*in == '/' || *in == '\\')
        in++;
    for (; *in && o + 1 < n; in++) {
        char c = *in;
        if (c == '\\')
            c = '/';
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        out[o++] = c;
    }
    out[o] = 0;
    /* strip trailing slashes */
    while (o > 0 && out[o - 1] == '/') {
        out[--o] = 0;
    }
}

int path_equal(const char *a, const char *b)
{
    char na[VOL_PATH_MAX], nb[VOL_PATH_MAX];
    path_normalize(a, na, sizeof na);
    path_normalize(b, nb, sizeof nb);
    return strcmp(na, nb) == 0;
}

int path_join(char *dst, size_t n, const char *prefix, const char *name)
{
    if (!prefix || !prefix[0]) {
        if (snprintf(dst, n, "/%s", name) >= (int)n)
            return -1;
        return 0;
    }
    if (snprintf(dst, n, "%s/%s", prefix, name) >= (int)n)
        return -1;
    return 0;
}

const char *path_basename(const char *p)
{
    const char *s = p;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\')
            s = p + 1;
    }
    return s;
}

void fat_83_to_name(const uint8_t name[11], char *out, size_t n)
{
    char base[9], ext[4];
    memcpy(base, name, 8);
    memcpy(ext, name + 8, 3);
    base[8] = 0;
    ext[3] = 0;
    if ((uint8_t)base[0] == 0x05)
        base[0] = (char)0xE5;
    for (int i = 7; i >= 0 && base[i] == ' '; i--)
        base[i] = 0;
    for (int i = 2; i >= 0 && ext[i] == ' '; i--)
        ext[i] = 0;
    if (ext[0])
        snprintf(out, n, "%s.%s", base, ext);
    else
        snprintf(out, n, "%s", base);
}

int fat_name_valid_char(unsigned char c)
{
    if (c < 0x20)
        return 0;
    return !strchr("\"*+,./:;<=>?[\\]|", c);
}

void fat_make_short_name(const char *longname, uint8_t out[11], int seq)
{
    char base[9] = "        ";
    char ext[4] = "   ";
    const char *bn = path_basename(longname);
    const char *dot = strrchr(bn, '.');
    int bi = 0, ei = 0;
    const char *p = bn;
    const char *end = (dot && dot != bn) ? dot : bn + strlen(bn);

    memset(out, ' ', 11);
    for (; p < end && bi < 8; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '.')
            continue;
        if (!fat_name_valid_char(c))
            c = '_';
        c = (unsigned char)toupper(c);
        base[bi++] = (char)c;
    }
    if (dot && dot[1]) {
        p = dot + 1;
        for (; *p && ei < 3; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == ' ')
                continue;
            if (!fat_name_valid_char(c))
                c = '_';
            c = (unsigned char)toupper(c);
            ext[ei++] = (char)c;
        }
    }
    if (seq > 0) {
        char tilde[8];
        int tlen = snprintf(tilde, sizeof tilde, "~%d", seq);
        int keep = 8 - tlen;
        if (keep < 1)
            keep = 1;
        if (bi > keep)
            bi = keep;
        memcpy(base + bi, tilde, (size_t)tlen);
        bi += tlen;
    }
    memcpy(out, base, 8);
    memcpy(out + 8, ext, 3);
}

uint32_t count_extents(const uint32_t *cl, uint32_t n)
{
    uint32_t e;
    if (n == 0)
        return 0;
    e = 1;
    for (uint32_t i = 1; i < n; i++) {
        if (cl[i] != cl[i - 1] + 1)
            e++;
    }
    return e;
}

void warn_backup(void)
{
    fprintf(stderr,
            "WARNING: Backup your data before any write operation.\n"
            "Recommend running fsck on the volume first. The device must be unmounted.\n");
}
