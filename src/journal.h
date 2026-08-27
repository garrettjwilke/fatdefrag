#ifndef FATDEFRAG_JOURNAL_H
#define FATDEFRAG_JOURNAL_H

#include <stdint.h>
#include "device.h"

#define JNL_MAGIC "FDJRNLv1"

#define JNL_NONE           0
#define JNL_START          1
#define JNL_DATA_COPIED    2
#define JNL_NEW_ALLOCATED  3
#define JNL_DIRENT_UPDATED 4

typedef struct {
    char magic[8];
    uint32_t serial;
    uint32_t phase;
    uint32_t fs_kind; /* 32 or 64 for exfat */
    uint32_t old_first;
    uint32_t new_first;
    uint32_t nclusters;
    uint32_t old_no_fat_chain;
    uint32_t new_no_fat_chain;
    uint64_t dirent_off;
    uint64_t file_entry_off;
    uint8_t secondary_count;
    uint8_t pad[7];
    uint32_t crc;
} Journal;

uint32_t journal_crc(const Journal *j);
int journal_valid(const Journal *j, uint32_t serial);
int journal_read(Device *d, uint64_t lba, uint32_t bps, Journal *j);
int journal_write(Device *d, uint64_t lba, uint32_t bps, const Journal *j);
int journal_clear(Device *d, uint64_t lba, uint32_t bps);

#endif
