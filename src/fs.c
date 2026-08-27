#include "fs.h"

#include "fat32.h"
#include "exfat.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fl_push(FileList *fl, const FileRec *r)
{
    if (fl->n == fl->cap) {
        size_t ncap = fl->cap ? fl->cap * 2 : 16;
        FileRec *ni = realloc(fl->items, ncap * sizeof *ni);
        if (!ni)
            return -1;
        fl->items = ni;
        fl->cap = ncap;
    }
    fl->items[fl->n++] = *r;
    return 0;
}

void fl_free(FileList *fl)
{
    free(fl->items);
    fl->items = NULL;
    fl->n = fl->cap = 0;
}

struct Volume {
    Device *dev;
    FsKind kind;
    Fat32 *fat32;
    Exfat *exfat;
};

int vol_open(Device *dev, Volume **out)
{
    uint8_t boot[512];
    Volume *v;
    if (dev_read(dev, 0, boot, 512) != 0)
        return -1;
    v = calloc(1, sizeof *v);
    if (!v)
        return -1;
    v->dev = dev;
    if (exfat_probe(boot)) {
        v->kind = FS_EXFAT;
        if (exfat_open(dev, boot, &v->exfat) != 0) {
            free(v);
            return -1;
        }
    } else if (fat32_probe(boot)) {
        v->kind = FS_FAT32;
        if (fat32_open(dev, boot, &v->fat32) != 0) {
            free(v);
            return -1;
        }
    } else {
        fprintf(stderr, "%s: not a FAT32 or exFAT volume (pass a partition or superfloppy image)\n",
                dev_path(dev));
        free(v);
        return -1;
    }
    *out = v;
    return 0;
}

void vol_close(Volume *v)
{
    if (!v)
        return;
    fat32_close(v->fat32);
    exfat_close(v->exfat);
    free(v);
}

FsKind vol_kind(const Volume *v) { return v->kind; }

const char *vol_kind_name(const Volume *v)
{
    return v->kind == FS_EXFAT ? "exFAT" : "FAT32";
}

int vol_recover(Volume *v)
{
    if (v->kind == FS_EXFAT)
        return exfat_recover(v->exfat);
    return fat32_recover(v->fat32);
}

int vol_journal_dirty(Volume *v)
{
    if (v->kind == FS_EXFAT)
        return exfat_journal_dirty(v->exfat);
    return fat32_journal_dirty(v->fat32);
}

int vol_list(Volume *v, FileList *fl)
{
    if (v->kind == FS_EXFAT)
        return exfat_list(v->exfat, fl);
    return fat32_list(v->fat32, fl);
}

int vol_check(Volume *v, const char *filter, int verbose, CheckResult *res)
{
    FileList fl = {0};
    uint32_t worst = 0;
    uint64_t frag = 0, files = 0;
    int found = 0;

    memset(res, 0, sizeof *res);
    res->kind = vol_kind_name(v);
    if (v->kind == FS_EXFAT) {
        res->serial = exfat_serial(v->exfat);
        res->cluster_bytes = exfat_cluster_bytes(v->exfat);
    } else {
        res->serial = fat32_serial(v->fat32);
        res->cluster_bytes = fat32_cluster_bytes(v->fat32);
    }
    if (vol_list(v, &fl) != 0) {
        fl_free(&fl);
        return -1;
    }
    for (size_t i = 0; i < fl.n; i++) {
        FileRec *f = &fl.items[i];
        if (filter && !path_equal(filter, f->path))
            continue;
        found = 1;
        files++;
        if (f->extents > 1)
            frag++;
        if (f->extents > worst)
            worst = f->extents;
        if (verbose || filter)
            printf("  %s  size=%llu  clusters=%u  extents=%u%s\n", f->path,
                   (unsigned long long)f->size, f->nclusters, f->extents,
                   f->extents > 1 ? "  [fragmented]" : "");
    }
    if (filter && !found) {
        fprintf(stderr, "file not found: %s\n", filter);
        fl_free(&fl);
        return -1;
    }
    res->files = files;
    res->fragmented = frag;
    res->worst_extents = worst;
    fl_free(&fl);
    return 0;
}

int vol_defrag(Volume *v, const char *filter, int dry_run, int verbose)
{
    if (v->kind == FS_EXFAT)
        return exfat_defrag(v->exfat, filter, dry_run, verbose);
    return fat32_defrag(v->fat32, filter, dry_run, verbose);
}

int vol_copy(Volume *v, const char *host_src, const char *vol_dst, int dry_run, int verbose)
{
    if (v->kind == FS_EXFAT)
        return exfat_copy(v->exfat, host_src, vol_dst, dry_run, verbose);
    return fat32_copy(v->fat32, host_src, vol_dst, dry_run, verbose);
}
