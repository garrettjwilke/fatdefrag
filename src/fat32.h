#ifndef FATDEFRAG_FAT32_H
#define FATDEFRAG_FAT32_H

#include "device.h"
#include "fs.h"

typedef struct Fat32 Fat32;

int fat32_probe(const uint8_t boot[512]);
int fat32_open(Device *dev, const uint8_t boot[512], Fat32 **out);
void fat32_close(Fat32 *fs);

uint32_t fat32_serial(const Fat32 *fs);
uint32_t fat32_cluster_bytes(const Fat32 *fs);

int fat32_recover(Fat32 *fs);
int fat32_journal_dirty(Fat32 *fs);
int fat32_list(Fat32 *fs, FileList *fl);
int fat32_defrag(Fat32 *fs, const char *filter, int dry_run, int verbose);
int fat32_copy(Fat32 *fs, const char *host_src, const char *vol_dst, int dry_run, int verbose);

#endif
