#ifndef FATDEFRAG_EXFAT_H
#define FATDEFRAG_EXFAT_H

#include "device.h"
#include "fs.h"

typedef struct Exfat Exfat;

int exfat_probe(const uint8_t boot[512]);
int exfat_open(Device *dev, const uint8_t boot[512], Exfat **out);
void exfat_close(Exfat *fs);

uint32_t exfat_serial(const Exfat *fs);
uint32_t exfat_cluster_bytes(const Exfat *fs);

int exfat_recover(Exfat *fs);
int exfat_journal_dirty(Exfat *fs);
int exfat_list(Exfat *fs, FileList *fl);
int exfat_defrag(Exfat *fs, const char *filter, int dry_run, int verbose);
int exfat_copy(Exfat *fs, const char *host_src, const char *vol_dst, int dry_run, int verbose);

#endif
