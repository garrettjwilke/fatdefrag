/* Minimal valid exFAT image with an optional fragmented file. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BPS 512
#define SPC 8
#define TOTSEC 32768
#define FATOFF 24
#define FATSZ 32
#define HEAP 64
#define NCL 4088
#define CLUSZ (BPS * SPC)
#define ROOT 35
#define BMCL 2
#define UPCL 3
#define UPNCL 32

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t cl_off(uint32_t cl)
{
    return (HEAP + (cl - 2) * SPC) * BPS;
}

static uint32_t boot_checksum(const uint8_t *s, int n)
{
    uint32_t c = 0;
    for (int i = 0; i < n; i++) {
        if (i == 106 || i == 107 || i == 112)
            continue;
        c = ((c & 1u) ? 0x80000000u : 0) + (c >> 1) + s[i];
    }
    return c;
}

static uint16_t entry_checksum(const uint8_t *ents, int nent)
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

static uint16_t name_hash(const uint16_t *name, int len)
{
    uint16_t hash = 0;
    for (int i = 0; i < len; i++) {
        uint16_t c = name[i];
        if (c >= 'a' && c <= 'z')
            c = (uint16_t)(c - 32);
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (c & 0xFF));
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + (c >> 8));
    }
    return hash;
}

static void bm_set(uint8_t *bm, uint32_t cl)
{
    uint32_t i = cl - 2;
    bm[i / 8] |= (uint8_t)(1u << (i % 8));
}

static void put_name(uint8_t *ent, const char *ascii)
{
    ent[0] = 0xC1;
    for (int i = 0; i < 15; i++) {
        uint16_t ch = ascii[i] ? (uint8_t)ascii[i] : 0;
        wr16(ent + 2 + i * 2, ch);
        if (!ascii[i])
            break;
    }
}

static void put_file_set(uint8_t *dst, const char *name, uint32_t first, uint64_t size, int no_fat)
{
    int nlen = (int)strlen(name);
    uint16_t uname[32];
    uint8_t set[96];
    memset(set, 0, sizeof set);
    for (int i = 0; i < nlen; i++)
        uname[i] = (uint8_t)name[i];
    set[0] = 0x85;
    set[1] = 2;
    wr16(set + 4, 0x0020);
    set[32] = 0xC0;
    set[33] = (uint8_t)(no_fat ? 0x03 : 0x01);
    set[35] = (uint8_t)nlen;
    wr16(set + 36, name_hash(uname, nlen));
    wr64(set + 40, size);
    wr32(set + 52, first);
    wr64(set + 56, size);
    put_name(set + 64, name);
    wr16(set + 2, entry_checksum(set, 3));
    memcpy(dst, set, 96);
}

int main(int argc, char **argv)
{
    const char *path;
    int empty = 0;
    FILE *fp;
    size_t imgsz = (size_t)TOTSEC * BPS;
    uint8_t *img, *fat, *bm, *root;
    uint32_t csum, i;
    uint32_t gap_clus[] = {40, 42, 44};

    if (argc < 2) {
        fprintf(stderr, "usage: mkexfat [--empty] IMAGE\n");
        return 1;
    }
    path = argv[argc - 1];
    for (i = 1; (int)i < argc - 1; i++)
        if (strcmp(argv[i], "--empty") == 0)
            empty = 1;

    img = calloc(1, imgsz);
    if (!img)
        return 1;

    img[0] = 0xEB;
    img[1] = 0x76;
    img[2] = 0x90;
    memcpy(img + 3, "EXFAT   ", 8);
    wr64(img + 64, 0);
    wr64(img + 72, TOTSEC);
    wr32(img + 80, FATOFF);
    wr32(img + 84, FATSZ);
    wr32(img + 88, HEAP);
    wr32(img + 92, NCL);
    wr32(img + 96, ROOT);
    wr32(img + 100, 0xABCDEF01);
    wr16(img + 104, 0x0100);
    wr16(img + 106, 0);
    img[108] = 9;
    img[109] = 3;
    img[110] = 1;
    img[111] = 0x80;
    img[112] = 0xFF;
    img[510] = 0x55;
    img[511] = 0xAA;
    for (i = 1; i <= 8; i++) {
        img[i * BPS + 510] = 0x55;
        img[i * BPS + 511] = 0xAA;
    }
    csum = boot_checksum(img, 11 * BPS);
    for (i = 0; i < BPS; i += 4)
        wr32(img + 11 * BPS + i, csum);
    memcpy(img + 12 * BPS, img, 12 * BPS);

    fat = img + FATOFF * BPS;
    wr32(fat + 0, 0xFFFFFFF8);
    wr32(fat + 4, 0xFFFFFFFF);
    wr32(fat + ROOT * 4, 0xFFFFFFFF);

    bm = img + cl_off(BMCL);
    for (i = 2; i <= ROOT; i++)
        bm_set(bm, i);

    for (i = 0; i < 65536; i++)
        wr16(img + cl_off(UPCL) + i * 2, (uint16_t)i);

    root = img + cl_off(ROOT);
    /* bitmap entry */
    root[0] = 0x81;
    wr32(root + 20, BMCL);
    wr64(root + 24, (NCL + 7) / 8);
    /* upcase */
    root[32] = 0x82;
    wr32(root + 52, UPCL);
    wr64(root + 56, 131072);

    if (!empty) {
        uint32_t gsz = 3 * CLUSZ;
        put_file_set(root + 64, "FILLER.BIN", 36, 4 * CLUSZ, 1);
        for (i = 36; i <= 39; i++) {
            bm_set(bm, i);
            memset(img + cl_off(i), 'F', CLUSZ);
        }
        put_file_set(root + 160, "GAP.BIN", 40, gsz, 0);
        for (i = 0; i < 3; i++) {
            uint32_t cl = gap_clus[i];
            uint32_t next = (i + 1 < 3) ? gap_clus[i + 1] : 0xFFFFFFFF;
            uint32_t b;
            wr32(fat + cl * 4, next);
            bm_set(bm, cl);
            for (b = 0; b < CLUSZ; b++)
                img[cl_off(cl) + b] = (uint8_t)((i * CLUSZ + b) & 0xFF);
        }
    }

    fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        free(img);
        return 1;
    }
    if (fwrite(img, 1, imgsz, fp) != imgsz) {
        perror("write");
        fclose(fp);
        free(img);
        return 1;
    }
    fclose(fp);
    free(img);
    printf("wrote %s (%s exFAT)\n", path, empty ? "empty" : "fragmented");
    return 0;
}
