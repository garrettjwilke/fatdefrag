#include "exfat.h"

#include "journal.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#define EXFAT_EOC 0xFFFFFFF8u
#define EXFAT_BAD 0xFFFFFFF7u
#define EX_TYPE_EOD     0x00
#define EX_TYPE_BITMAP  0x81
#define EX_TYPE_UPCASE  0x82
#define EX_TYPE_LABEL   0x83
#define EX_TYPE_FILE    0x85
#define EX_TYPE_STREAM  0xC0
#define EX_TYPE_NAME    0xC1

#define EX_ATTR_DIR 0x0010

struct Exfat {
    Device *dev;
    uint32_t bps;
    uint32_t spc;
    uint32_t cluster_bytes;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t nfats;
    uint32_t heap_offset;
    uint32_t cluster_count;
    uint32_t max_cl;
    uint32_t root_clus;
    uint32_t serial;
    uint8_t *fat;
    uint32_t fat_bytes;
    int fat_dirty;
    uint8_t *bitmap;
    uint32_t bitmap_bytes;
    uint32_t bitmap_cluster;
    int bm_dirty;
    uint16_t *upcase;
    uint32_t upcase_len;
    uint64_t journal_lba;
    int have_journal;
};

int exfat_probe(const uint8_t boot[512])
{
    uint8_t bps_shift, spc_shift;
    if (memcmp(boot + 3, "EXFAT   ", 8) != 0)
        return 0;
    bps_shift = boot[108];
    spc_shift = boot[109];
    if (bps_shift < 9 || bps_shift > 12)
        return 0;
    if (spc_shift > 25)
        return 0;
    return 1;
}

static uint32_t fat_get(const Exfat *fs, uint32_t cl)
{
    return rd_le32(fs->fat + (size_t)cl * 4);
}

static void fat_set(Exfat *fs, uint32_t cl, uint32_t val)
{
    wr_le32(fs->fat + (size_t)cl * 4, val);
    fs->fat_dirty = 1;
}

static int fat_is_eoc(uint32_t v)
{
    return v >= EXFAT_EOC;
}

static uint64_t cluster_off(const Exfat *fs, uint32_t cl)
{
    return ((uint64_t)fs->heap_offset + (uint64_t)(cl - 2) * fs->spc) * fs->bps;
}

static int read_cluster(Exfat *fs, uint32_t cl, void *buf)
{
    if (cl < 2 || cl > fs->max_cl)
        return -1;
    return dev_read(fs->dev, cluster_off(fs, cl), buf, fs->cluster_bytes);
}

static int write_cluster(Exfat *fs, uint32_t cl, const void *buf)
{
    if (cl < 2 || cl > fs->max_cl)
        return -1;
    return dev_write(fs->dev, cluster_off(fs, cl), buf, fs->cluster_bytes);
}

static int flush_fat(Exfat *fs)
{
    if (!fs->fat_dirty)
        return 0;
    for (uint32_t i = 0; i < fs->nfats; i++) {
        uint64_t off = ((uint64_t)fs->fat_offset + (uint64_t)i * fs->fat_length) * fs->bps;
        if (dev_write(fs->dev, off, fs->fat, fs->fat_bytes) != 0)
            return -1;
    }
    fs->fat_dirty = 0;
    return dev_sync(fs->dev);
}

static int bm_used(const Exfat *fs, uint32_t cl)
{
    uint32_t i = cl - 2;
    if (i / 8 >= fs->bitmap_bytes)
        return 1;
    return (fs->bitmap[i / 8] >> (i % 8)) & 1;
}

static void bm_set(Exfat *fs, uint32_t cl, int used)
{
    uint32_t i = cl - 2;
    if (i / 8 >= fs->bitmap_bytes)
        return;
    if (used)
        fs->bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
    else
        fs->bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
    fs->bm_dirty = 1;
}

static int flush_bitmap(Exfat *fs)
{
    uint32_t ncl, i;
    uint8_t *buf;
    uint32_t remain;
    if (!fs->bm_dirty)
        return 0;
    ncl = (fs->bitmap_bytes + fs->cluster_bytes - 1) / fs->cluster_bytes;
    buf = malloc(fs->cluster_bytes);
    if (!buf)
        return -1;
    remain = fs->bitmap_bytes;
    for (i = 0; i < ncl; i++) {
        uint32_t chunk = remain > fs->cluster_bytes ? fs->cluster_bytes : remain;
        memset(buf, 0, fs->cluster_bytes);
        memcpy(buf, fs->bitmap + (size_t)i * fs->cluster_bytes, chunk);
        if (write_cluster(fs, fs->bitmap_cluster + i, buf) != 0) {
            free(buf);
            return -1;
        }
        remain -= chunk;
    }
    free(buf);
    fs->bm_dirty = 0;
    return dev_sync(fs->dev);
}

static int walk_fat_chain(Exfat *fs, uint32_t first, uint32_t **out, uint32_t *n_out)
{
    uint32_t cap = 8, n = 0, cl = first;
    uint32_t *arr;
    if (first < 2) {
        *out = NULL;
        *n_out = 0;
        return 0;
    }
    arr = malloc(cap * sizeof *arr);
    if (!arr)
        return -1;
    while (cl >= 2 && cl <= fs->max_cl && !fat_is_eoc(cl) && cl != EXFAT_BAD) {
        if (n == cap) {
            uint32_t *na = realloc(arr, cap * 2 * sizeof *na);
            if (!na) {
                free(arr);
                return -1;
            }
            arr = na;
            cap *= 2;
        }
        arr[n++] = cl;
        if (n > fs->cluster_count + 2) {
            free(arr);
            return -1;
        }
        cl = fat_get(fs, cl);
        if (cl == 0)
            break;
    }
    *out = arr;
    *n_out = n;
    return 0;
}

static int sequential_clusters(uint32_t first, uint32_t n, uint32_t **out)
{
    uint32_t *arr = malloc((size_t)n * sizeof *arr);
    if (!arr)
        return -1;
    for (uint32_t i = 0; i < n; i++)
        arr[i] = first + i;
    *out = arr;
    return 0;
}

static int load_chain_bytes(Exfat *fs, uint32_t first, int no_fat, uint64_t data_len,
                            uint8_t **data, uint32_t **cls, uint32_t *ncl)
{
    uint32_t n, i;
    uint8_t *buf;
    uint32_t *c = NULL;
    uint32_t expect = 0;
    if (data_len)
        expect = (uint32_t)((data_len + fs->cluster_bytes - 1) / fs->cluster_bytes);
    if (first < 2) {
        *data = calloc(1, 1);
        *cls = NULL;
        *ncl = 0;
        return *data ? 0 : -1;
    }
    if (!no_fat) {
        uint32_t fat0 = fat_get(fs, first);
        if (fat0 == 0 || (expect > 1 && fat_is_eoc(fat0)))
            no_fat = 1;
    }
    if (no_fat) {
        n = expect ? expect : 1;
        if (sequential_clusters(first, n, &c) != 0)
            return -1;
    } else {
        if (walk_fat_chain(fs, first, &c, &n) != 0)
            return -1;
    }
    buf = calloc(1, (size_t)n * fs->cluster_bytes + 1);
    if (n && !buf) {
        free(c);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (read_cluster(fs, c[i], buf + (size_t)i * fs->cluster_bytes) != 0) {
            free(buf);
            free(c);
            return -1;
        }
    }
    *data = buf;
    *cls = c;
    *ncl = n;
    return 0;
}

static uint16_t upc(const Exfat *fs, uint16_t c)
{
    if (fs->upcase && c < fs->upcase_len)
        return fs->upcase[c];
    if (c >= 'a' && c <= 'z')
        return (uint16_t)(c - 32);
    return c;
}

static uint16_t name_hash(const Exfat *fs, const uint16_t *name, int len)
{
    uint16_t hash = 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = upc(fs, name[i]);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (c & 0xFF));
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (c >> 8));
    }
    return hash;
}

static uint16_t entryset_checksum(const uint8_t *ents, int nent)
{
    uint16_t sum = 0;
    int nbytes = nent * 32;
    for (int i = 0; i < nbytes; i++) {
        if (i == 2 || i == 3)
            continue;
        sum = (uint16_t)(((sum << 15) | (sum >> 1)) + ents[i]);
    }
    return sum;
}

static int decompress_upcase(const uint8_t *data, size_t len, uint16_t *out)
{
    size_t i = 0;
    uint32_t index = 0;
    while (index < 65536)
        out[index] = (uint16_t)index, index++;
    index = 0;
    while (i + 2 <= len && index < 65536) {
        uint16_t c = rd_le16(data + i);
        i += 2;
        if (c == 0xFFFF && i + 2 <= len) {
            uint16_t n = rd_le16(data + i);
            i += 2;
            index += n;
        } else {
            out[index++] = c;
        }
    }
    return 0;
}

static int load_meta(Exfat *fs)
{
    uint8_t *root = NULL;
    uint32_t *rcls = NULL, rn = 0, e;
    uint32_t root_bytes;

    if (load_chain_bytes(fs, fs->root_clus, 0, 0, &root, &rcls, &rn) != 0)
        return -1;
    root_bytes = rn * fs->cluster_bytes;

    for (e = 0; e + 32 <= root_bytes; e += 32) {
        uint8_t *ent = root + e;
        uint8_t t = ent[0];
        if (t == EX_TYPE_EOD)
            break;
        if (t == EX_TYPE_BITMAP) {
            uint32_t first = rd_le32(ent + 20);
            uint64_t dlen = rd_le64(ent + 24);
            uint32_t ncl;
            uint8_t *tmp = NULL;
            uint32_t *bcls = NULL;
            fs->bitmap_cluster = first;
            fs->bitmap_bytes = (fs->cluster_count + 7) / 8;
            if (dlen < fs->bitmap_bytes)
                fs->bitmap_bytes = (uint32_t)dlen;
            fs->bitmap = calloc(1, fs->bitmap_bytes);
            if (!fs->bitmap) {
                free(root);
                free(rcls);
                return -1;
            }
            if (load_chain_bytes(fs, first, 0, dlen, &tmp, &bcls, &ncl) != 0) {
                /* bitmap often contiguous; try sequential via NoFatChain */
                free(tmp);
                free(bcls);
                if (load_chain_bytes(fs, first, 1, dlen, &tmp, &bcls, &ncl) != 0) {
                    free(root);
                    free(rcls);
                    return -1;
                }
            }
            memcpy(fs->bitmap, tmp, fs->bitmap_bytes < (uint32_t)ncl * fs->cluster_bytes
                                        ? fs->bitmap_bytes
                                        : (uint32_t)ncl * fs->cluster_bytes);
            free(tmp);
            free(bcls);
        } else if (t == EX_TYPE_UPCASE) {
            uint32_t first = rd_le32(ent + 20);
            uint64_t dlen = rd_le64(ent + 24);
            uint8_t *tmp = NULL;
            uint32_t *ucls = NULL, ncl = 0;
            fs->upcase = malloc(65536 * sizeof *fs->upcase);
            if (!fs->upcase) {
                free(root);
                free(rcls);
                return -1;
            }
            fs->upcase_len = 65536;
            if (load_chain_bytes(fs, first, 1, dlen, &tmp, &ucls, &ncl) != 0) {
                free(tmp);
                free(ucls);
                tmp = NULL;
                ucls = NULL;
                if (load_chain_bytes(fs, first, 0, dlen, &tmp, &ucls, &ncl) != 0) {
                    free(fs->upcase);
                    fs->upcase = NULL;
                    continue;
                }
            }
            decompress_upcase(tmp, (size_t)dlen, fs->upcase);
            free(tmp);
            free(ucls);
        }
    }
    free(root);
    free(rcls);
    if (!fs->bitmap) {
        fprintf(stderr, "exFAT: allocation bitmap not found\n");
        return -1;
    }
    return 0;
}

int exfat_open(Device *dev, const uint8_t boot[512], Exfat **out)
{
    Exfat *fs = calloc(1, sizeof *fs);
    uint64_t fat_end;
    if (!fs)
        return -1;
    fs->dev = dev;
    fs->bps = 1u << boot[108];
    fs->spc = 1u << boot[109];
    fs->cluster_bytes = fs->bps * fs->spc;
    fs->fat_offset = rd_le32(boot + 80);
    fs->fat_length = rd_le32(boot + 84);
    fs->heap_offset = rd_le32(boot + 88);
    fs->cluster_count = rd_le32(boot + 92);
    fs->root_clus = rd_le32(boot + 96);
    fs->serial = rd_le32(boot + 100);
    fs->nfats = boot[110] ? boot[110] : 1;
    fs->max_cl = fs->cluster_count + 1;
    fs->fat_bytes = fs->fat_length * fs->bps;
    if (fs->cluster_bytes == 0 || fs->cluster_bytes > 32u * 1024u * 1024u) {
        fprintf(stderr, "exFAT: unsupported cluster size\n");
        free(fs);
        return -1;
    }
    fs->fat = malloc(fs->fat_bytes);
    if (!fs->fat) {
        free(fs);
        return -1;
    }
    if (dev_read(dev, (uint64_t)fs->fat_offset * fs->bps, fs->fat, fs->fat_bytes) != 0) {
        free(fs->fat);
        free(fs);
        return -1;
    }
    fat_end = (uint64_t)fs->fat_offset + (uint64_t)fs->fat_length * fs->nfats;
    if (fs->heap_offset > fat_end) {
        fs->journal_lba = fat_end;
        fs->have_journal = 1;
    }
    if (load_meta(fs) != 0) {
        free(fs->fat);
        free(fs->bitmap);
        free(fs->upcase);
        free(fs);
        return -1;
    }
    *out = fs;
    return 0;
}

void exfat_close(Exfat *fs)
{
    if (!fs)
        return;
    free(fs->fat);
    free(fs->bitmap);
    free(fs->upcase);
    free(fs);
}

uint32_t exfat_serial(const Exfat *fs) { return fs->serial; }
uint32_t exfat_cluster_bytes(const Exfat *fs) { return fs->cluster_bytes; }

static int file_clusters(Exfat *fs, uint32_t first, int no_fat, uint64_t size,
                         uint32_t **out, uint32_t *n_out)
{
    uint32_t n;
    if (first < 2) {
        *out = NULL;
        *n_out = 0;
        return 0;
    }
    if (no_fat) {
        n = size ? (uint32_t)((size + fs->cluster_bytes - 1) / fs->cluster_bytes) : 0;
        if (n && sequential_clusters(first, n, out) != 0)
            return -1;
        *n_out = n;
        return 0;
    }
    return walk_fat_chain(fs, first, out, n_out);
}

static int collect_dir(Exfat *fs, uint32_t first, int no_fat, uint64_t dlen,
                       const char *prefix, FileList *fl);

static int parse_dir_buf(Exfat *fs, uint8_t *buf, uint32_t nbytes, const uint32_t *cls,
                         uint32_t dir_first, int dir_nofat, uint64_t dir_dlen,
                         const char *prefix, FileList *fl)
{
    uint32_t e = 0;
    while (e + 32 <= nbytes) {
        uint8_t *ent = buf + e;
        uint8_t t = ent[0];
        uint8_t sec, si;
        uint16_t attr;
        uint8_t *stream = NULL;
        char name[256];
        uint16_t uname[256];
        int ulen = 0;
        uint32_t first_cl;
        uint64_t dlen;
        int no_fat;
        uint64_t file_off, stream_off;
        FileRec rec;
        uint32_t *chain = NULL, ncl = 0;

        if (t == EX_TYPE_EOD)
            break;
        if ((t & 0x80) == 0) {
            e += 32;
            continue;
        }
        if (t != EX_TYPE_FILE) {
            e += 32;
            continue;
        }
        sec = ent[1];
        if (e + (uint32_t)(1 + sec) * 32 > nbytes)
            break;
        attr = rd_le16(ent + 4);
        file_off = cluster_off(fs, cls[e / fs->cluster_bytes]) + (e % fs->cluster_bytes);
        stream_off = 0;
        memset(uname, 0, sizeof uname);
        for (si = 1; si <= sec; si++) {
            uint8_t *s = buf + e + (uint32_t)si * 32;
            if (s[0] == EX_TYPE_STREAM) {
                stream = s;
                stream_off = cluster_off(fs, cls[(e + si * 32) / fs->cluster_bytes]) +
                             ((e + si * 32) % fs->cluster_bytes);
            } else if (s[0] == EX_TYPE_NAME) {
                for (int k = 0; k < 15 && ulen < 255; k++)
                    uname[ulen++] = rd_le16(s + 2 + k * 2);
            }
        }
        if (!stream) {
            e += (uint32_t)(1 + sec) * 32;
            continue;
        }
        if (stream[3] < ulen)
            ulen = stream[3];
        utf16le_to_utf8(uname, ulen, name, sizeof name);
        first_cl = rd_le32(stream + 20);
        dlen = rd_le64(stream + 24);
        no_fat = (stream[1] & 0x02) ? 1 : 0;

        if (attr & EX_ATTR_DIR) {
            char sub[VOL_PATH_MAX];
            if (name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0) {
                if (path_join(sub, sizeof sub, prefix, name) != 0)
                    return -1;
                if (collect_dir(fs, first_cl, no_fat, dlen, sub, fl) != 0)
                    return -1;
            }
            e += (uint32_t)(1 + sec) * 32;
            continue;
        }

        memset(&rec, 0, sizeof rec);
        if (path_join(rec.path, sizeof rec.path, prefix, name) != 0)
            return -1;
        rec.first_cluster = first_cl;
        rec.size = dlen;
        rec.dirent_off = stream_off;
        rec.file_entry_off = file_off;
        rec.no_fat_chain = (uint8_t)no_fat;
        rec.secondary_count = sec;
        rec.dir_first = dir_first;
        rec.dir_nofat = (uint8_t)dir_nofat;
        rec.dir_dlen = dir_dlen;
        rec.dir_index = e / 32;
        if (file_clusters(fs, first_cl, no_fat, dlen, &chain, &ncl) != 0)
            return -1;
        rec.nclusters = ncl;
        rec.extents = count_extents(chain, ncl);
        free(chain);
        if (fl_push(fl, &rec) != 0)
            return -1;
        e += (uint32_t)(1 + sec) * 32;
    }
    return 0;
}

static int collect_dir(Exfat *fs, uint32_t first, int no_fat, uint64_t dlen,
                       const char *prefix, FileList *fl)
{
    uint8_t *buf = NULL;
    uint32_t *cls = NULL, ncl = 0;
    int rc;
    if (load_chain_bytes(fs, first, no_fat, dlen, &buf, &cls, &ncl) != 0)
        return -1;
    rc = parse_dir_buf(fs, buf, ncl * fs->cluster_bytes, cls, first, no_fat, dlen, prefix, fl);
    free(buf);
    free(cls);
    return rc;
}

int exfat_list(Exfat *fs, FileList *fl)
{
    return collect_dir(fs, fs->root_clus, 0, 0, "", fl);
}

static int find_free_run(Exfat *fs, uint32_t n, uint32_t *out)
{
    uint32_t run = 0, start = 0;
    if (n == 0)
        return -1;
    for (uint32_t cl = 2; cl <= fs->max_cl; cl++) {
        if (!bm_used(fs, cl)) {
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

static int jnl_put(Exfat *fs, uint32_t phase, const FileRec *f, uint32_t new_first)
{
    Journal j;
    if (!fs->have_journal)
        return 0;
    memset(&j, 0, sizeof j);
    memcpy(j.magic, JNL_MAGIC, 8);
    j.serial = fs->serial;
    j.phase = phase;
    j.fs_kind = FS_EXFAT;
    j.old_first = f->first_cluster;
    j.new_first = new_first;
    j.nclusters = f->nclusters;
    j.old_no_fat_chain = f->no_fat_chain;
    j.new_no_fat_chain = 1;
    j.dirent_off = f->dirent_off;
    j.file_entry_off = f->file_entry_off;
    j.secondary_count = f->secondary_count;
    return journal_write(fs->dev, fs->journal_lba, fs->bps, &j);
}

static int mark_run(Exfat *fs, uint32_t start, uint32_t n, int used)
{
    for (uint32_t i = 0; i < n; i++)
        bm_set(fs, start + i, used);
    return flush_bitmap(fs);
}

static int free_file_clusters(Exfat *fs, uint32_t first, uint32_t n, int no_fat)
{
    if (first < 2 || n == 0)
        return 0;
    if (no_fat) {
        return mark_run(fs, first, n, 0);
    } else {
        uint32_t cl = first;
        for (uint32_t i = 0; i < n && cl >= 2 && cl <= fs->max_cl; i++) {
            uint32_t next = fat_get(fs, cl);
            fat_set(fs, cl, 0);
            bm_set(fs, cl, 0);
            if (fat_is_eoc(next) || next == 0)
                break;
            cl = next;
        }
        if (flush_fat(fs) != 0)
            return -1;
        return flush_bitmap(fs);
    }
}

static int rewrite_stream_first(Exfat *fs, const FileRec *f, uint32_t new_first)
{
    uint8_t *buf = NULL;
    uint32_t *cls = NULL, ncl = 0;
    int nent = 1 + f->secondary_count;
    uint32_t e = f->dir_index * 32;
    uint8_t *set, *stream = NULL;
    uint16_t sum;
    uint32_t i;

    if (load_chain_bytes(fs, f->dir_first, f->dir_nofat, f->dir_dlen, &buf, &cls, &ncl) != 0)
        return -1;
    if (e + (uint32_t)nent * 32 > ncl * fs->cluster_bytes) {
        free(buf);
        free(cls);
        return -1;
    }
    set = buf + e;
    for (int s = 1; s < nent; s++) {
        if (set[s * 32] == EX_TYPE_STREAM) {
            stream = set + s * 32;
            break;
        }
    }
    if (!stream) {
        free(buf);
        free(cls);
        return -1;
    }
    wr_le32(stream + 20, new_first);
    stream[1] |= 0x03;
    sum = entryset_checksum(set, nent);
    wr_le16(set + 2, sum);
    for (i = 0; i < ncl; i++) {
        if (write_cluster(fs, cls[i], buf + (size_t)i * fs->cluster_bytes) != 0) {
            free(buf);
            free(cls);
            return -1;
        }
    }
    free(buf);
    free(cls);
    return dev_sync(fs->dev);
}

static int copy_clusters(Exfat *fs, const uint32_t *old, uint32_t n, uint32_t neu)
{
    uint8_t *buf = malloc(fs->cluster_bytes);
    if (!buf)
        return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (read_cluster(fs, old[i], buf) != 0 || write_cluster(fs, neu + i, buf) != 0) {
            free(buf);
            return -1;
        }
    }
    free(buf);
    return dev_sync(fs->dev);
}

static int relocate_file(Exfat *fs, FileRec *f, int dry, int verbose)
{
    uint32_t *old = NULL, n = 0, dest;
    int old_nofat = f->no_fat_chain;
    if (f->nclusters == 0 || f->extents <= 1)
        return 0;
    if (file_clusters(fs, f->first_cluster, f->no_fat_chain, f->size, &old, &n) != 0)
        return -1;
    f->nclusters = n;
    if (find_free_run(fs, n, &dest) != 0) {
        fprintf(stderr, "  no contiguous hole of %u clusters for %s\n", n, f->path);
        free(old);
        return -1;
    }
    if (verbose || dry)
        printf("  %s  %u clusters  %u extents -> cluster %u%s\n",
               f->path, n, f->extents, dest, dry ? " (dry-run)" : "");
    if (dry) {
        free(old);
        return 0;
    }
    if (jnl_put(fs, JNL_START, f, dest) != 0) {
        free(old);
        return -1;
    }
    if (copy_clusters(fs, old, n, dest) != 0) {
        free(old);
        return -1;
    }
    if (jnl_put(fs, JNL_DATA_COPIED, f, dest) != 0) {
        free(old);
        return -1;
    }
    if (mark_run(fs, dest, n, 1) != 0) {
        free(old);
        return -1;
    }
    if (jnl_put(fs, JNL_NEW_ALLOCATED, f, dest) != 0) {
        free(old);
        return -1;
    }
    if (rewrite_stream_first(fs, f, dest) != 0) {
        free(old);
        return -1;
    }
    f->first_cluster = dest;
    f->no_fat_chain = 1;
    if (jnl_put(fs, JNL_DIRENT_UPDATED, f, dest) != 0) {
        free(old);
        return -1;
    }
    (void)old_nofat;
    for (uint32_t i = 0; i < n; i++) {
        bm_set(fs, old[i], 0);
        fat_set(fs, old[i], 0);
    }
    if (flush_fat(fs) != 0 || flush_bitmap(fs) != 0) {
        free(old);
        return -1;
    }
    if (fs->have_journal)
        journal_clear(fs->dev, fs->journal_lba, fs->bps);
    free(old);
    return 0;
}

int exfat_journal_dirty(Exfat *fs)
{
    Journal j;
    if (!fs->have_journal)
        return 0;
    if (journal_read(fs->dev, fs->journal_lba, fs->bps, &j) != 0)
        return 0;
    return journal_valid(&j, fs->serial);
}

int exfat_recover(Exfat *fs)
{
    Journal j;
    FileRec f;
    if (!fs->have_journal)
        return 0;
    if (journal_read(fs->dev, fs->journal_lba, fs->bps, &j) != 0)
        return 0;
    if (!journal_valid(&j, fs->serial))
        return 0;
    fprintf(stderr, "Recovering interrupted exFAT operation (phase %u)...\n", j.phase);
    memset(&f, 0, sizeof f);
    f.first_cluster = j.old_first;
    f.nclusters = j.nclusters;
    f.dirent_off = j.dirent_off;
    f.file_entry_off = j.file_entry_off;
    f.secondary_count = j.secondary_count;
    f.no_fat_chain = (uint8_t)j.old_no_fat_chain;

    switch (j.phase) {
    case JNL_START:
    case JNL_DATA_COPIED:
        return journal_clear(fs->dev, fs->journal_lba, fs->bps);
    case JNL_NEW_ALLOCATED:
        if (rewrite_stream_first(fs, &f, j.new_first) != 0)
            return -1;
        /* fall through */
    case JNL_DIRENT_UPDATED:
        if (free_file_clusters(fs, j.old_first, j.nclusters, (int)j.old_no_fat_chain) != 0)
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

int exfat_defrag(Exfat *fs, const char *filter, int dry_run, int verbose)
{
    FileList fl = {0};
    int rc = 0, did = 0, failed = 0;
    if (exfat_list(fs, &fl) != 0) {
        fl_free(&fl);
        return -1;
    }
    qsort(fl.items, fl.n, sizeof *fl.items, rec_cmp_size);
    for (size_t i = 0; i < fl.n; i++) {
        FileRec *f = &fl.items[i];
        if (filter && !path_equal(filter, f->path))
            continue;
        if (f->extents <= 1)
            continue;
        if (relocate_file(fs, f, dry_run, verbose) != 0) {
            failed++;
            rc = -1;
            if (filter)
                break;
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
    fl_free(&fl);
    return rc;
}

static int lookup_dir(Exfat *fs, uint32_t first, int no_fat, uint64_t dlen, const char *want,
                      uint32_t *clus_out, int *no_fat_out, uint64_t *dlen_out)
{
    uint8_t *buf = NULL;
    uint32_t *cls = NULL, ncl = 0, e, nbytes;
    int rc = -1;
    if (load_chain_bytes(fs, first, no_fat, dlen, &buf, &cls, &ncl) != 0)
        return -1;
    nbytes = ncl * fs->cluster_bytes;
    for (e = 0; e + 32 <= nbytes; ) {
        uint8_t *ent = buf + e;
        uint8_t t = ent[0], sec, si;
        uint16_t attr;
        uint8_t *stream = NULL;
        char name[256];
        uint16_t uname[256];
        int ulen = 0;
        if (t == EX_TYPE_EOD)
            break;
        if ((t & 0x80) == 0 || t != EX_TYPE_FILE) {
            e += 32;
            continue;
        }
        sec = ent[1];
        attr = rd_le16(ent + 4);
        for (si = 1; si <= sec; si++) {
            uint8_t *s = buf + e + si * 32;
            if (s[0] == EX_TYPE_STREAM)
                stream = s;
            else if (s[0] == EX_TYPE_NAME) {
                for (int k = 0; k < 15 && ulen < 255; k++)
                    uname[ulen++] = rd_le16(s + 2 + k * 2);
            }
        }
        if (stream && (stream[3] < ulen))
            ulen = stream[3];
        utf16le_to_utf8(uname, ulen, name, sizeof name);
        if ((attr & EX_ATTR_DIR) && path_equal(name, want) && stream) {
            *clus_out = rd_le32(stream + 20);
            *no_fat_out = (stream[1] & 0x02) ? 1 : 0;
            *dlen_out = rd_le64(stream + 24);
            rc = 0;
            break;
        }
        e += (uint32_t)(1 + sec) * 32;
    }
    free(buf);
    free(cls);
    return rc;
}

static int find_dir_slots(Exfat *fs, uint32_t first, int no_fat, uint64_t dlen, int need,
                          uint64_t *off_out)
{
    uint8_t *buf = NULL;
    uint32_t *cls = NULL, ncl = 0, nbytes, e;
    int run = 0;
    uint64_t run_off = 0;
    if (load_chain_bytes(fs, first, no_fat, dlen, &buf, &cls, &ncl) != 0)
        return -1;
    nbytes = ncl * fs->cluster_bytes;
    for (e = 0; e + 32 <= nbytes; e += 32) {
        uint8_t t = buf[e];
        int is_free = (t == EX_TYPE_EOD || (t & 0x80) == 0);
        if (is_free) {
            if (run == 0)
                run_off = cluster_off(fs, cls[e / fs->cluster_bytes]) + (e % fs->cluster_bytes);
            run++;
            if (run >= need) {
                *off_out = run_off;
                free(buf);
                free(cls);
                return 0;
            }
            if (t == EX_TYPE_EOD) {
                uint32_t remain = (nbytes - e) / 32;
                if (run + (int)remain - 1 >= need) {
                    *off_out = run_off;
                    free(buf);
                    free(cls);
                    return 0;
                }
            }
        } else {
            run = 0;
        }
    }
    /* grow root-like: allocate one cluster and FAT-link from last */
    {
        uint32_t neu, last;
        uint8_t *z;
        if (ncl == 0 || find_free_run(fs, 1, &neu) != 0) {
            free(buf);
            free(cls);
            return -1;
        }
        z = calloc(1, fs->cluster_bytes);
        if (!z) {
            free(buf);
            free(cls);
            return -1;
        }
        if (write_cluster(fs, neu, z) != 0) {
            free(z);
            free(buf);
            free(cls);
            return -1;
        }
        free(z);
        last = cls[ncl - 1];
        fat_set(fs, last, neu);
        fat_set(fs, neu, 0xFFFFFFFF);
        bm_set(fs, neu, 1);
        flush_fat(fs);
        flush_bitmap(fs);
        *off_out = cluster_off(fs, neu);
        free(buf);
        free(cls);
        return 0;
    }
}

int exfat_copy(Exfat *fs, const char *host_src, const char *vol_dst, int dry_run, int verbose)
{
    struct stat st;
    int fd, ulen, n_name, nent;
    uint64_t size, remain;
    uint32_t ncl = 0, dest = 0, dir_cl;
    int dir_nofat = 0;
    uint64_t dir_dlen = 0;
    char parent[VOL_PATH_MAX], leaf[256], norm[VOL_PATH_MAX];
    char *slash;
    uint16_t utf16[256];
    uint8_t *set, *clu;
    uint64_t slot;
    uint16_t hash, sum;

    if (stat(host_src, &st) != 0) {
        fprintf(stderr, "stat %s: %s\n", host_src, strerror(errno));
        return -1;
    }
    size = (uint64_t)st.st_size;
    ncl = size ? (uint32_t)((size + fs->cluster_bytes - 1) / fs->cluster_bytes) : 0;

    path_normalize(vol_dst, norm, sizeof norm);
    slash = strrchr(norm, '/');
    if (slash) {
        *slash = 0;
        snprintf(parent, sizeof parent, "%s", norm);
        snprintf(leaf, sizeof leaf, "%s", slash + 1);
    } else {
        parent[0] = 0;
        snprintf(leaf, sizeof leaf, "%s", norm);
    }
    dir_cl = fs->root_clus;
    dir_nofat = 0;
    dir_dlen = 0;
    if (parent[0]) {
        char tmp[VOL_PATH_MAX], *save, *tok;
        snprintf(tmp, sizeof tmp, "%s", parent);
        tok = strtok_r(tmp, "/", &save);
        while (tok) {
            uint32_t next = 0;
            int nf = 0;
            uint64_t dl = 0;
            if (lookup_dir(fs, dir_cl, dir_nofat, dir_dlen, tok, &next, &nf, &dl) != 0) {
                fprintf(stderr, "directory not found: %s\n", tok);
                return -1;
            }
            dir_cl = next;
            dir_nofat = nf;
            dir_dlen = dl;
            tok = strtok_r(NULL, "/", &save);
        }
    }
    if (ncl && find_free_run(fs, ncl, &dest) != 0) {
        fprintf(stderr, "no contiguous space for %u clusters\n", ncl);
        return -1;
    }
    if (dry_run || verbose)
        printf("copy %s -> %s (%llu bytes, %u clusters)%s\n", host_src, vol_dst,
               (unsigned long long)size, ncl, dry_run ? " [dry-run]" : "");
    if (dry_run)
        return 0;

    ulen = utf8_to_utf16le(leaf, utf16, 255);
    n_name = (ulen + 14) / 15;
    if (n_name < 1)
        n_name = 1;
    nent = 1 + 1 + n_name; /* file + stream + names */
    if (find_dir_slots(fs, dir_cl, dir_nofat, dir_dlen, nent, &slot) != 0) {
        fprintf(stderr, "no directory space\n");
        return -1;
    }

    fd = open(host_src, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", host_src, strerror(errno));
        return -1;
    }
    clu = calloc(1, fs->cluster_bytes);
    if (!clu) {
        close(fd);
        return -1;
    }
    remain = size;
    for (uint32_t i = 0; i < ncl; i++) {
        size_t chunk = remain > fs->cluster_bytes ? fs->cluster_bytes : (size_t)remain;
        memset(clu, 0, fs->cluster_bytes);
        if (chunk && read(fd, clu, chunk) != (ssize_t)chunk) {
            free(clu);
            close(fd);
            return -1;
        }
        remain -= chunk;
        if (write_cluster(fs, dest + i, clu) != 0) {
            free(clu);
            close(fd);
            return -1;
        }
    }
    close(fd);
    free(clu);
    if (ncl && mark_run(fs, dest, ncl, 1) != 0)
        return -1;

    hash = name_hash(fs, utf16, ulen);
    set = calloc((size_t)nent, 32);
    if (!set)
        return -1;
    set[0] = EX_TYPE_FILE;
    set[1] = (uint8_t)(1 + n_name);
    wr_le16(set + 4, 0x0020); /* archive */
    set[32] = EX_TYPE_STREAM;
    set[33] = 0x03; /* AllocPossible | NoFatChain */
    set[35] = (uint8_t)ulen;
    wr_le16(set + 36, hash);
    wr_le64(set + 40, size);
    wr_le32(set + 52, dest);
    wr_le64(set + 56, size);
    for (int ni = 0; ni < n_name; ni++) {
        uint8_t *ne = set + (2 + ni) * 32;
        ne[0] = EX_TYPE_NAME;
        for (int k = 0; k < 15; k++) {
            int idx = ni * 15 + k;
            uint16_t ch = (idx < ulen) ? utf16[idx] : 0;
            wr_le16(ne + 2 + k * 2, ch);
        }
    }
    sum = entryset_checksum(set, nent);
    wr_le16(set + 2, sum);
    if (dev_write(fs->dev, slot, set, (size_t)nent * 32) != 0) {
        free(set);
        return -1;
    }
    free(set);
    return dev_sync(fs->dev);
}
