#include "fat32.h"

#include "journal.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

#define FAT_EOC  0x0FFFFFF8u
#define FAT_BAD  0x0FFFFFF7u
#define ATTR_RO  0x01
#define ATTR_HID 0x02
#define ATTR_SYS 0x04
#define ATTR_VOL 0x08
#define ATTR_DIR 0x10
#define ATTR_ARC 0x20
#define ATTR_LFN 0x0F

struct Fat32 {
    Device *dev;
    uint32_t bps;
    uint32_t spc;
    uint32_t reserved;
    uint32_t nfats;
    uint32_t fatsz;
    uint32_t root_clus;
    uint32_t fsinfo;
    uint32_t serial;
    uint32_t first_data;
    uint32_t cluster_count; /* number of data clusters */
    uint32_t max_cl;        /* last valid cluster index (= cluster_count + 1) */
    uint32_t cluster_bytes;
    uint8_t *fat;
    uint32_t fat_bytes;
    int fat_dirty;
    uint64_t journal_lba;
    int have_journal;
};

int fat32_probe(const uint8_t boot[512])
{
    uint16_t bps, root_ent, fatsz16, sig;
    uint8_t spc, nfats;
    uint32_t fatsz32, tot16, tot32;
    if (boot[0] != 0xEB && boot[0] != 0xE9)
        return 0;
    if (memcmp(boot + 3, "EXFAT   ", 8) == 0)
        return 0;
    bps = rd_le16(boot + 11);
    spc = boot[13];
    nfats = boot[16];
    root_ent = rd_le16(boot + 17);
    tot16 = rd_le16(boot + 19);
    fatsz16 = rd_le16(boot + 22);
    tot32 = rd_le32(boot + 32);
    fatsz32 = rd_le32(boot + 36);
    sig = rd_le16(boot + 510);
    if (sig != 0xAA55)
        return 0;
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096)
        return 0;
    if (spc == 0 || (spc & (spc - 1)) != 0)
        return 0;
    if (nfats == 0)
        return 0;
    if (fatsz16 != 0 || fatsz32 == 0)
        return 0;
    if (root_ent != 0)
        return 0;
    if (tot16 == 0 && tot32 == 0)
        return 0;
    return 1;
}

static uint32_t fat_get(const Fat32 *fs, uint32_t cl)
{
    return rd_le32(fs->fat + (size_t)cl * 4) & 0x0FFFFFFFu;
}

static void fat_set(Fat32 *fs, uint32_t cl, uint32_t val)
{
    uint8_t *p = fs->fat + (size_t)cl * 4;
    uint32_t old = rd_le32(p);
    wr_le32(p, (old & 0xF0000000u) | (val & 0x0FFFFFFFu));
    fs->fat_dirty = 1;
}

static int fat_is_eoc(uint32_t v)
{
    return v >= FAT_EOC;
}

static int flush_fat(Fat32 *fs)
{
    if (!fs->fat_dirty)
        return 0;
    for (uint32_t i = 0; i < fs->nfats; i++) {
        uint64_t off = (uint64_t)(fs->reserved + i * fs->fatsz) * fs->bps;
        if (dev_write(fs->dev, off, fs->fat, fs->fat_bytes) != 0)
            return -1;
    }
    if (dev_sync(fs->dev) != 0)
        return -1;
    fs->fat_dirty = 0;
    return 0;
}

static uint64_t cluster_off(const Fat32 *fs, uint32_t cl)
{
    return ((uint64_t)fs->first_data + (uint64_t)(cl - 2) * fs->spc) * fs->bps;
}

static int read_cluster(Fat32 *fs, uint32_t cl, void *buf)
{
    if (cl < 2 || cl > fs->max_cl)
        return -1;
    return dev_read(fs->dev, cluster_off(fs, cl), buf, fs->cluster_bytes);
}

static int write_cluster(Fat32 *fs, uint32_t cl, const void *buf)
{
    if (cl < 2 || cl > fs->max_cl)
        return -1;
    return dev_write(fs->dev, cluster_off(fs, cl), buf, fs->cluster_bytes);
}

int fat32_open(Device *dev, const uint8_t boot[512], Fat32 **out)
{
    Fat32 *fs = calloc(1, sizeof *fs);
    uint32_t totsec, data_sec;
    if (!fs)
        return -1;
    fs->dev = dev;
    fs->bps = rd_le16(boot + 11);
    fs->spc = boot[13];
    fs->reserved = rd_le16(boot + 14);
    fs->nfats = boot[16];
    fs->fatsz = rd_le32(boot + 36);
    fs->root_clus = rd_le32(boot + 44);
    fs->fsinfo = rd_le16(boot + 48);
    fs->serial = rd_le32(boot + 67);
    totsec = rd_le16(boot + 19);
    if (totsec == 0)
        totsec = rd_le32(boot + 32);
    fs->first_data = fs->reserved + fs->nfats * fs->fatsz;
    if (totsec <= fs->first_data) {
        fprintf(stderr, "FAT32: invalid geometry\n");
        free(fs);
        return -1;
    }
    data_sec = totsec - fs->first_data;
    fs->cluster_count = data_sec / fs->spc;
    fs->max_cl = fs->cluster_count + 1;
    fs->cluster_bytes = fs->bps * fs->spc;
    fs->fat_bytes = fs->fatsz * fs->bps;
    if (fs->cluster_bytes == 0 || fs->cluster_bytes > 32u * 1024u * 1024u) {
        fprintf(stderr, "FAT32: unsupported cluster size %u\n", fs->cluster_bytes);
        free(fs);
        return -1;
    }
    fs->fat = malloc(fs->fat_bytes);
    if (!fs->fat) {
        fprintf(stderr, "FAT32: cannot allocate FAT (%u bytes)\n", fs->fat_bytes);
        free(fs);
        return -1;
    }
    if (dev_read(dev, (uint64_t)fs->reserved * fs->bps, fs->fat, fs->fat_bytes) != 0) {
        free(fs->fat);
        free(fs);
        return -1;
    }
    fs->have_journal = 0;
    if (fs->reserved > 9) {
        fs->journal_lba = fs->reserved - 1;
        fs->have_journal = 1;
    }
    *out = fs;
    return 0;
}

void fat32_close(Fat32 *fs)
{
    if (!fs)
        return;
    free(fs->fat);
    free(fs);
}

uint32_t fat32_serial(const Fat32 *fs) { return fs->serial; }
uint32_t fat32_cluster_bytes(const Fat32 *fs) { return fs->cluster_bytes; }

static int walk_chain(Fat32 *fs, uint32_t first, uint32_t expect, uint32_t **out, uint32_t *n_out)
{
    uint32_t cap = expect ? expect : 8;
    uint32_t n = 0;
    uint32_t cl = first;
    uint32_t *arr;
    if (first < 2) {
        *out = NULL;
        *n_out = 0;
        return 0;
    }
    arr = malloc((size_t)cap * sizeof *arr);
    if (!arr)
        return -1;
    while (cl >= 2 && cl <= fs->max_cl && !fat_is_eoc(cl) && cl != FAT_BAD) {
        if (n == cap) {
            uint32_t ncap = cap * 2;
            uint32_t *na = realloc(arr, (size_t)ncap * sizeof *na);
            if (!na) {
                free(arr);
                return -1;
            }
            arr = na;
            cap = ncap;
        }
        arr[n++] = cl;
        if (n > fs->cluster_count + 2) {
            fprintf(stderr, "FAT32: cycle in cluster chain\n");
            free(arr);
            return -1;
        }
        cl = fat_get(fs, cl);
    }
    *out = arr;
    *n_out = n;
    return 0;
}

static void lfn_store(uint16_t *buf, int seq, const uint8_t *ent)
{
    int base = (seq - 1) * 13;
    static const int offs[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    if (seq < 1 || base + 13 > 260)
        return;
    for (int i = 0; i < 13; i++)
        buf[base + i] = rd_le16(ent + offs[i]);
}

static int collect_dir(Fat32 *fs, uint32_t start, const char *prefix, FileList *fl);

static int handle_entry(Fat32 *fs, const uint8_t *ent, uint64_t ent_off,
                        const char *prefix, uint16_t *lfnbuf, int *have_lfn, FileList *fl)
{
    uint8_t attr = ent[11];
    uint8_t first = ent[0];
    char name[256];
    uint32_t cl;
    uint32_t *chain = NULL;
    uint32_t ncl = 0;
    FileRec rec;

    if (first == 0x00)
        return 1; /* end */
    if (first == 0xE5) {
        *have_lfn = 0;
        return 0;
    }
    if ((attr & ATTR_LFN) == ATTR_LFN) {
        int seq = first & 0x1F;
        if (first & 0x40)
            memset(lfnbuf, 0, 260 * sizeof *lfnbuf);
        lfn_store(lfnbuf, seq, ent);
        *have_lfn = 1;
        return 0;
    }
    if (attr & ATTR_VOL) {
        *have_lfn = 0;
        return 0;
    }

    if (*have_lfn && utf16le_to_utf8(lfnbuf, 256, name, sizeof name) > 0) {
        /* ok */
    } else {
        fat_83_to_name(ent, name, sizeof name);
    }
    *have_lfn = 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;

    cl = (uint32_t)rd_le16(ent + 26) | ((uint32_t)rd_le16(ent + 20) << 16);

    if (attr & ATTR_DIR) {
        char sub[VOL_PATH_MAX];
        if (path_join(sub, sizeof sub, prefix, name) != 0)
            return -1;
        if (cl >= 2)
            return collect_dir(fs, cl, sub, fl);
        return 0;
    }

    memset(&rec, 0, sizeof rec);
    if (path_join(rec.path, sizeof rec.path, prefix, name) != 0)
        return -1;
    rec.first_cluster = cl;
    rec.size = rd_le32(ent + 28);
    rec.dirent_off = ent_off;
    rec.no_fat_chain = 0;
    if (walk_chain(fs, cl, (uint32_t)((rec.size + fs->cluster_bytes - 1) / fs->cluster_bytes),
                   &chain, &ncl) != 0)
        return -1;
    rec.nclusters = ncl;
    rec.extents = count_extents(chain, ncl);
    free(chain);
    if (fl_push(fl, &rec) != 0)
        return -1;
    return 0;
}

static int collect_dir(Fat32 *fs, uint32_t start, const char *prefix, FileList *fl)
{
    uint32_t *dchain = NULL;
    uint32_t nd = 0, i, e;
    uint8_t *buf;
    uint16_t lfnbuf[260];
    int have_lfn = 0;
    int rc = 0;

    if (walk_chain(fs, start, 0, &dchain, &nd) != 0)
        return -1;
    buf = malloc(fs->cluster_bytes);
    if (!buf) {
        free(dchain);
        return -1;
    }
    memset(lfnbuf, 0, sizeof lfnbuf);

    for (i = 0; i < nd && rc == 0; i++) {
        if (read_cluster(fs, dchain[i], buf) != 0) {
            rc = -1;
            break;
        }
        for (e = 0; e < fs->cluster_bytes; e += 32) {
            uint64_t off = cluster_off(fs, dchain[i]) + e;
            int r = handle_entry(fs, buf + e, off, prefix, lfnbuf, &have_lfn, fl);
            if (r < 0) {
                rc = -1;
                break;
            }
            if (r > 0) {
                i = nd;
                break;
            }
        }
    }
    free(buf);
    free(dchain);
    return rc;
}

int fat32_list(Fat32 *fs, FileList *fl)
{
    return collect_dir(fs, fs->root_clus, "", fl);
}

static int find_free_run(Fat32 *fs, uint32_t n, uint32_t *out)
{
    uint32_t run = 0, start = 0;
    if (n == 0)
        return -1;
    for (uint32_t cl = 2; cl <= fs->max_cl; cl++) {
        uint32_t v = fat_get(fs, cl);
        if (v == 0) {
            if (run == 0)
                start = cl;
            run++;
            if (run >= n) {
                *out = start;
                return 0;
            }
        } else {
            run = 0;
        }
    }
    return -1;
}

static int jnl_put(Fat32 *fs, uint32_t phase, const FileRec *f, uint32_t new_first)
{
    Journal j;
    if (!fs->have_journal)
        return 0;
    memset(&j, 0, sizeof j);
    memcpy(j.magic, JNL_MAGIC, 8);
    j.serial = fs->serial;
    j.phase = phase;
    j.fs_kind = FS_FAT32;
    j.old_first = f->first_cluster;
    j.new_first = new_first;
    j.nclusters = f->nclusters;
    j.dirent_off = f->dirent_off;
    return journal_write(fs->dev, fs->journal_lba, fs->bps, &j);
}

static int update_dirent_cluster(Fat32 *fs, uint64_t off, uint32_t cl)
{
    uint8_t ent[32];
    if (dev_read(fs->dev, off, ent, 32) != 0)
        return -1;
    wr_le16(ent + 26, (uint16_t)(cl & 0xFFFF));
    wr_le16(ent + 20, (uint16_t)(cl >> 16));
    if (dev_write(fs->dev, off, ent, 32) != 0)
        return -1;
    return dev_sync(fs->dev);
}

static int free_chain(Fat32 *fs, uint32_t first, uint32_t n)
{
    uint32_t cl = first;
    for (uint32_t i = 0; i < n && cl >= 2 && cl <= fs->max_cl; i++) {
        uint32_t next = fat_get(fs, cl);
        fat_set(fs, cl, 0);
        if (fat_is_eoc(next))
            break;
        cl = next;
    }
    return flush_fat(fs);
}

static int link_run(Fat32 *fs, uint32_t start, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = (i + 1 < n) ? start + i + 1 : 0x0FFFFFFF;
        fat_set(fs, start + i, next);
    }
    return flush_fat(fs);
}

static int copy_clusters(Fat32 *fs, const uint32_t *old, uint32_t n, uint32_t new_start)
{
    uint8_t *buf = malloc(fs->cluster_bytes);
    if (!buf)
        return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (read_cluster(fs, old[i], buf) != 0 ||
            write_cluster(fs, new_start + i, buf) != 0) {
            free(buf);
            return -1;
        }
    }
    free(buf);
    return dev_sync(fs->dev);
}

static int relocate_file(Fat32 *fs, FileRec *f, int dry, int verbose)
{
    uint32_t *old = NULL, n = 0, dest;
    FileRec jf = *f;

    if (f->nclusters == 0 || f->extents <= 1)
        return 0;
    if (walk_chain(fs, f->first_cluster, f->nclusters, &old, &n) != 0)
        return -1;
    if (n != f->nclusters)
        f->nclusters = n;
    if (find_free_run(fs, f->nclusters, &dest) != 0) {
        fprintf(stderr, "  no contiguous hole of %u clusters for %s\n", f->nclusters, f->path);
        free(old);
        return -1;
    }
    if (verbose || dry)
        printf("  %s  %u clusters  %u extents -> cluster %u%s\n",
               f->path, f->nclusters, f->extents, dest, dry ? " (dry-run)" : "");
    if (dry) {
        free(old);
        return 0;
    }

    jf.nclusters = f->nclusters;
    if (jnl_put(fs, JNL_START, &jf, dest) != 0) {
        free(old);
        return -1;
    }
    if (copy_clusters(fs, old, f->nclusters, dest) != 0) {
        free(old);
        return -1;
    }
    if (jnl_put(fs, JNL_DATA_COPIED, &jf, dest) != 0) {
        free(old);
        return -1;
    }
    if (link_run(fs, dest, f->nclusters) != 0) {
        free(old);
        return -1;
    }
    if (jnl_put(fs, JNL_NEW_ALLOCATED, &jf, dest) != 0) {
        free(old);
        return -1;
    }
    if (update_dirent_cluster(fs, f->dirent_off, dest) != 0) {
        free(old);
        return -1;
    }
    f->first_cluster = dest;
    if (jnl_put(fs, JNL_DIRENT_UPDATED, &jf, dest) != 0) {
        free(old);
        return -1;
    }
    if (free_chain(fs, old[0], n) != 0) {
        free(old);
        return -1;
    }
    if (fs->have_journal && journal_clear(fs->dev, fs->journal_lba, fs->bps) != 0) {
        free(old);
        return -1;
    }
    free(old);
    return 0;
}

int fat32_journal_dirty(Fat32 *fs)
{
    Journal j;
    if (!fs->have_journal)
        return 0;
    if (journal_read(fs->dev, fs->journal_lba, fs->bps, &j) != 0)
        return 0;
    return journal_valid(&j, fs->serial);
}

int fat32_recover(Fat32 *fs)
{
    Journal j;
    FileRec f;
    if (!fs->have_journal)
        return 0;
    if (journal_read(fs->dev, fs->journal_lba, fs->bps, &j) != 0)
        return 0;
    if (!journal_valid(&j, fs->serial))
        return 0;
    fprintf(stderr, "Recovering interrupted FAT32 operation (phase %u)...\n", j.phase);
    memset(&f, 0, sizeof f);
    f.first_cluster = j.old_first;
    f.nclusters = j.nclusters;
    f.dirent_off = j.dirent_off;

    switch (j.phase) {
    case JNL_START:
    case JNL_DATA_COPIED:
        /* dirent still old; new clusters still free. Discard. */
        return journal_clear(fs->dev, fs->journal_lba, fs->bps);
    case JNL_NEW_ALLOCATED:
        if (update_dirent_cluster(fs, j.dirent_off, j.new_first) != 0)
            return -1;
        /* fall through */
    case JNL_DIRENT_UPDATED:
        if (free_chain(fs, j.old_first, j.nclusters) != 0)
            return -1;
        return journal_clear(fs->dev, fs->journal_lba, fs->bps);
    default:
        return journal_clear(fs->dev, fs->journal_lba, fs->bps);
    }
}

static int rec_cmp_size(const void *a, const void *b)
{
    const FileRec *fa = a, *fb = b;
    if (fa->nclusters < fb->nclusters)
        return -1;
    if (fa->nclusters > fb->nclusters)
        return 1;
    return 0;
}

int fat32_defrag(Fat32 *fs, const char *filter, int dry_run, int verbose)
{
    FileList fl = {0};
    int rc = 0, did = 0, skipped = 0, failed = 0;
    if (fat32_list(fs, &fl) != 0) {
        fl_free(&fl);
        return -1;
    }
    qsort(fl.items, fl.n, sizeof *fl.items, rec_cmp_size);
    for (size_t i = 0; i < fl.n; i++) {
        FileRec *f = &fl.items[i];
        if (filter && !path_equal(filter, f->path))
            continue;
        if (f->extents <= 1) {
            if (filter && verbose)
                printf("  %s is already contiguous\n", f->path);
            continue;
        }
        if (relocate_file(fs, f, dry_run, verbose) != 0) {
            failed++;
            rc = -1;
            if (filter)
                break;
            skipped++;
            continue;
        }
        did++;
    }
    if (filter && did == 0 && failed == 0) {
        int found = 0;
        for (size_t i = 0; i < fl.n; i++)
            if (path_equal(filter, fl.items[i].path))
                found = 1;
        if (!found) {
            fprintf(stderr, "file not found: %s\n", filter);
            rc = -1;
        }
    }
    if (verbose)
        printf("defrag: relocated %d file(s)%s\n", did, dry_run ? " (dry-run)" : "");
    (void)skipped;
    fl_free(&fl);
    return rc;
}

static uint8_t lfn_checksum(const uint8_t name[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

static int short_name_taken(Fat32 *fs, uint32_t dir_cl, const uint8_t name[11])
{
    uint32_t *ch = NULL, n = 0;
    uint8_t *buf;
    int taken = 0;
    if (walk_chain(fs, dir_cl, 0, &ch, &n) != 0)
        return 1;
    buf = malloc(fs->cluster_bytes);
    if (!buf) {
        free(ch);
        return 1;
    }
    for (uint32_t i = 0; i < n && !taken; i++) {
        if (read_cluster(fs, ch[i], buf) != 0) {
            taken = 1;
            break;
        }
        for (uint32_t e = 0; e < fs->cluster_bytes; e += 32) {
            if (buf[e] == 0x00)
                break;
            if (buf[e] == 0xE5)
                continue;
            if ((buf[e + 11] & ATTR_LFN) == ATTR_LFN)
                continue;
            if (memcmp(buf + e, name, 11) == 0) {
                taken = 1;
                break;
            }
        }
    }
    free(buf);
    free(ch);
    return taken;
}

static int grow_dir(Fat32 *fs, uint32_t *last_cl)
{
    uint32_t neu;
    uint8_t *z;
    if (find_free_run(fs, 1, &neu) != 0)
        return -1;
    z = calloc(1, fs->cluster_bytes);
    if (!z)
        return -1;
    if (write_cluster(fs, neu, z) != 0) {
        free(z);
        return -1;
    }
    free(z);
    fat_set(fs, *last_cl, neu);
    fat_set(fs, neu, 0x0FFFFFFF);
    *last_cl = neu;
    return flush_fat(fs);
}

static int find_dir_slots(Fat32 *fs, uint32_t dir_cl, int need, uint64_t *off_out, uint32_t *last_cl_out)
{
    uint32_t *ch = NULL, n = 0;
    uint8_t *buf;
    int run = 0;
    uint64_t run_off = 0;
    uint32_t last = dir_cl;

    if (walk_chain(fs, dir_cl, 0, &ch, &n) != 0)
        return -1;
    buf = malloc(fs->cluster_bytes);
    if (!buf) {
        free(ch);
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        last = ch[i];
        if (read_cluster(fs, ch[i], buf) != 0) {
            free(buf);
            free(ch);
            return -1;
        }
        for (uint32_t e = 0; e < fs->cluster_bytes; e += 32) {
            int free_ent = (buf[e] == 0x00 || buf[e] == 0xE5);
            if (free_ent) {
                if (run == 0)
                    run_off = cluster_off(fs, ch[i]) + e;
                run++;
                if (run >= need) {
                    *off_out = run_off;
                    *last_cl_out = last;
                    free(buf);
                    free(ch);
                    return 0;
                }
                if (buf[e] == 0x00) {
                    /* rest of cluster is free */
                    uint32_t remain = (fs->cluster_bytes - e) / 32;
                    if (run + (int)remain - 1 >= need) {
                        *off_out = run_off;
                        *last_cl_out = last;
                        free(buf);
                        free(ch);
                        return 0;
                    }
                }
            } else {
                run = 0;
            }
        }
    }
    /* grow directory */
    if (grow_dir(fs, &last) != 0) {
        free(buf);
        free(ch);
        return -1;
    }
    *off_out = cluster_off(fs, last);
    *last_cl_out = last;
    free(buf);
    free(ch);
    return 0;
}

static void put_lfn_entry(uint8_t *ent, int seq, int last, const uint16_t *utf16, int ulen, uint8_t chk)
{
    static const int offs[] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int base = (seq - 1) * 13;
    memset(ent, 0, 32);
    ent[0] = (uint8_t)(seq | (last ? 0x40 : 0));
    ent[11] = ATTR_LFN;
    ent[13] = chk;
    for (int i = 0; i < 13; i++) {
        uint16_t ch;
        int idx = base + i;
        if (idx < ulen)
            ch = utf16[idx];
        else if (idx == ulen)
            ch = 0;
        else
            ch = 0xFFFF;
        wr_le16(ent + offs[i], ch);
    }
}

static int write_dir_entries(Fat32 *fs, uint64_t off, const uint8_t *ents, int nent)
{
    return dev_write(fs->dev, off, ents, (size_t)nent * 32);
}

static int lookup_dir(Fat32 *fs, uint32_t start, const char *want, uint32_t *clus_out, int want_dir)
{
    FileList fl = {0};
    int rc = -1;
    /* reuse collect? it only stores files. Walk locally. */
    uint32_t *ch = NULL, nd = 0;
    uint8_t *buf;
    uint16_t lfnbuf[260];
    int have_lfn = 0;

    (void)fl;
    if (walk_chain(fs, start, 0, &ch, &nd) != 0)
        return -1;
    buf = malloc(fs->cluster_bytes);
    if (!buf) {
        free(ch);
        return -1;
    }
    memset(lfnbuf, 0, sizeof lfnbuf);
    for (uint32_t i = 0; i < nd; i++) {
        if (read_cluster(fs, ch[i], buf) != 0)
            break;
        for (uint32_t e = 0; e < fs->cluster_bytes; e += 32) {
            uint8_t *ent = buf + e;
            uint8_t attr, first;
            char name[256];
            uint32_t cl;
            first = ent[0];
            if (first == 0x00) {
                i = nd;
                break;
            }
            if (first == 0xE5) {
                have_lfn = 0;
                continue;
            }
            attr = ent[11];
            if ((attr & ATTR_LFN) == ATTR_LFN) {
                int seq = first & 0x1F;
                if (first & 0x40)
                    memset(lfnbuf, 0, sizeof lfnbuf);
                lfn_store(lfnbuf, seq, ent);
                have_lfn = 1;
                continue;
            }
            if (attr & ATTR_VOL) {
                have_lfn = 0;
                continue;
            }
            if (have_lfn && utf16le_to_utf8(lfnbuf, 256, name, sizeof name) > 0) {
            } else {
                fat_83_to_name(ent, name, sizeof name);
            }
            have_lfn = 0;
            if (!path_equal(name, want))
                continue;
            cl = (uint32_t)rd_le16(ent + 26) | ((uint32_t)rd_le16(ent + 20) << 16);
            if (want_dir && !(attr & ATTR_DIR))
                continue;
            if (!want_dir && (attr & ATTR_DIR))
                continue;
            *clus_out = cl;
            rc = 0;
            i = nd;
            break;
        }
    }
    free(buf);
    free(ch);
    return rc;
}

static int split_parent(const char *vol_dst, char *parent, size_t pn, char *leaf, size_t ln)
{
    char norm[VOL_PATH_MAX];
    char *slash;
    path_normalize(vol_dst, norm, sizeof norm);
    slash = strrchr(norm, '/');
    if (!slash) {
        snprintf(parent, pn, "");
        snprintf(leaf, ln, "%s", norm);
        return 0;
    }
    *slash = 0;
    snprintf(parent, pn, "%s", norm);
    snprintf(leaf, ln, "%s", slash + 1);
    return 0;
}

int fat32_copy(Fat32 *fs, const char *host_src, const char *vol_dst, int dry_run, int verbose)
{
    struct stat st;
    int fd;
    uint64_t size;
    uint32_t ncl, dest, dir_cl;
    char parent[VOL_PATH_MAX], leaf[256];
    uint8_t shortn[11];
    uint16_t utf16[256];
    int ulen, n_lfn, nent, seq;
    uint8_t *ents, *clu;
    uint64_t slot_off;
    uint32_t last_cl;
    uint64_t remain;
    uint32_t i;

    if (stat(host_src, &st) != 0) {
        fprintf(stderr, "stat %s: %s\n", host_src, strerror(errno));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "%s: not a regular file\n", host_src);
        return -1;
    }
    size = (uint64_t)st.st_size;
    ncl = size ? (uint32_t)((size + fs->cluster_bytes - 1) / fs->cluster_bytes) : 0;

    split_parent(vol_dst, parent, sizeof parent, leaf, sizeof leaf);
    if (!leaf[0]) {
        fprintf(stderr, "invalid destination path\n");
        return -1;
    }
    dir_cl = fs->root_clus;
    if (parent[0]) {
        /* walk each component */
        char tmp[VOL_PATH_MAX], *save, *tok;
        snprintf(tmp, sizeof tmp, "%s", parent);
        tok = strtok_r(tmp, "/", &save);
        while (tok) {
            uint32_t next = 0;
            if (lookup_dir(fs, dir_cl, tok, &next, 1) != 0) {
                fprintf(stderr, "directory not found: %s\n", tok);
                return -1;
            }
            dir_cl = next;
            tok = strtok_r(NULL, "/", &save);
        }
    }

    if (ncl && find_free_run(fs, ncl, &dest) != 0) {
        fprintf(stderr, "no contiguous space for %u clusters\n", ncl);
        return -1;
    }
    if (dry_run || verbose)
        printf("copy %s -> %s (%llu bytes, %u clusters)%s\n",
               host_src, vol_dst, (unsigned long long)size, ncl,
               dry_run ? " [dry-run]" : "");
    if (dry_run)
        return 0;

    fd = open(host_src, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", host_src, strerror(errno));
        return -1;
    }

    seq = 0;
    do {
        fat_make_short_name(leaf, shortn, seq);
        seq++;
        if (seq > 999999) {
            close(fd);
            return -1;
        }
    } while (short_name_taken(fs, dir_cl, shortn));

    ulen = utf8_to_utf16le(leaf, utf16, 255);
    n_lfn = (ulen + 12) / 13;
    if (n_lfn < 1)
        n_lfn = 1;
    nent = n_lfn + 1;
    if (find_dir_slots(fs, dir_cl, nent, &slot_off, &last_cl) != 0) {
        fprintf(stderr, "no directory space\n");
        close(fd);
        return -1;
    }

    ents = calloc((size_t)nent, 32);
    if (!ents) {
        close(fd);
        return -1;
    }
    {
        uint8_t chk = lfn_checksum(shortn);
        for (int s = n_lfn; s >= 1; s--)
            put_lfn_entry(ents + (n_lfn - s) * 32, s, s == n_lfn, utf16, ulen, chk);
    }
    memcpy(ents + n_lfn * 32, shortn, 11);
    ents[n_lfn * 32 + 11] = ATTR_ARC;
    wr_le16(ents + n_lfn * 32 + 26, (uint16_t)(dest & 0xFFFF));
    wr_le16(ents + n_lfn * 32 + 20, (uint16_t)(dest >> 16));
    wr_le32(ents + n_lfn * 32 + 28, (uint32_t)(size > 0xFFFFFFFFull ? 0xFFFFFFFFu : size));

    clu = calloc(1, fs->cluster_bytes);
    if (!clu) {
        free(ents);
        close(fd);
        return -1;
    }
    remain = size;
    for (i = 0; i < ncl; i++) {
        size_t chunk = remain > fs->cluster_bytes ? fs->cluster_bytes : (size_t)remain;
        memset(clu, 0, fs->cluster_bytes);
        if (chunk) {
            ssize_t r = read(fd, clu, chunk);
            if (r != (ssize_t)chunk) {
                fprintf(stderr, "short read from %s\n", host_src);
                free(clu);
                free(ents);
                close(fd);
                return -1;
            }
            remain -= chunk;
        }
        if (write_cluster(fs, dest + i, clu) != 0) {
            free(clu);
            free(ents);
            close(fd);
            return -1;
        }
    }
    close(fd);
    free(clu);

    if (ncl && link_run(fs, dest, ncl) != 0) {
        free(ents);
        return -1;
    }
    if (write_dir_entries(fs, slot_off, ents, nent) != 0) {
        free(ents);
        return -1;
    }
    free(ents);
    return dev_sync(fs->dev);
}
