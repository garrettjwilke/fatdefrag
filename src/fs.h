#ifndef FATDEFRAG_FS_H
#define FATDEFRAG_FS_H

#include "device.h"
#include "../include/fatdefrag.h"

typedef enum { FS_UNKNOWN = 0, FS_FAT32 = 32, FS_EXFAT = 64 } FsKind;

typedef struct Volume Volume;

int vol_open(Device *dev, Volume **out);
void vol_close(Volume *v);

FsKind vol_kind(const Volume *v);
const char *vol_kind_name(const Volume *v);

int vol_recover(Volume *v);
int vol_journal_dirty(Volume *v);

int vol_list(Volume *v, FileList *fl);
int vol_check(Volume *v, const char *filter, int verbose, CheckResult *res);
int vol_defrag(Volume *v, const char *filter, int dry_run, int verbose);
int vol_copy(Volume *v, const char *host_src, const char *vol_dst, int dry_run, int verbose);

#endif
