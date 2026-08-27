#include "journal.h"
#include "util.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t journal_crc(const Journal *j)
{
    const uint8_t *p = (const uint8_t *)j;
    size_t n = offsetof(Journal, crc);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(c & 1));
    }
    return c ^ 0xFFFFFFFFu;
}

int journal_valid(const Journal *j, uint32_t serial)
{
    if (memcmp(j->magic, JNL_MAGIC, 8) != 0)
        return 0;
    if (j->crc != journal_crc(j))
        return 0;
    if (serial && j->serial != serial)
        return 0;
    if (j->phase == JNL_NONE)
        return 0;
    return 1;
}

int journal_read(Device *d, uint64_t lba, uint32_t bps, Journal *j)
{
    uint8_t *buf;
    int rc;
    if (bps < sizeof *j)
        return -1;
    buf = calloc(1, bps);
    if (!buf)
        return -1;
    rc = dev_read(d, lba * (uint64_t)bps, buf, bps);
    if (rc == 0)
        memcpy(j, buf, sizeof *j);
    free(buf);
    return rc;
}

int journal_write(Device *d, uint64_t lba, uint32_t bps, const Journal *j)
{
    Journal tmp = *j;
    uint8_t *buf;
    int rc;
    tmp.crc = journal_crc(&tmp);
    buf = calloc(1, bps);
    if (!buf)
        return -1;
    memcpy(buf, &tmp, sizeof tmp);
    rc = dev_write(d, lba * (uint64_t)bps, buf, bps);
    free(buf);
    if (rc == 0)
        rc = dev_sync(d);
    return rc;
}

int journal_clear(Device *d, uint64_t lba, uint32_t bps)
{
    uint8_t *buf = calloc(1, bps);
    int rc;
    if (!buf)
        return -1;
    rc = dev_write(d, lba * (uint64_t)bps, buf, bps);
    free(buf);
    if (rc == 0)
        rc = dev_sync(d);
    return rc;
}
