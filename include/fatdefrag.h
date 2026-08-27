#ifndef FATDEFRAG_H
#define FATDEFRAG_H

#include <stdint.h>
#include <stddef.h>

#define VOL_PATH_MAX 1024

typedef struct Device Device;
typedef struct Volume Volume;

typedef struct {
    char path[VOL_PATH_MAX];
    uint32_t first_cluster;
    uint64_t size;
    uint32_t nclusters;
    uint32_t extents;
    uint64_t dirent_off;
    uint64_t file_entry_off;
    uint8_t no_fat_chain;
    uint8_t secondary_count;
    uint32_t dir_first;
    uint8_t dir_nofat;
    uint64_t dir_dlen;
    uint32_t dir_index;
} FileRec;

typedef struct {
    FileRec *items;
    size_t n, cap;
} FileList;

typedef struct {
    const char *kind;
    uint32_t serial;
    uint32_t cluster_bytes;
    uint64_t files;
    uint64_t fragmented;
    uint32_t worst_extents;
} CheckResult;

int fl_push(FileList *fl, const FileRec *r);
void fl_free(FileList *fl);

int cmd_check(const char *device, const char *file, int verbose);
int cmd_defrag(const char *device, const char *file, int dry_run, int verbose);
int cmd_copy(const char *source, const char *dest_spec, int dry_run, int verbose);

#endif
