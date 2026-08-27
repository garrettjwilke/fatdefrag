/* Build a tiny FAT32 image with a fragmented file (no mkfs required). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BPS 512
#define SPC 8
#define RSV 32
#define NFAT 2
#define FATSZ 64
#define TOTSEC 65536
#define ROOT 2
#define CLUSZ (BPS * SPC)

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t cl_off(uint32_t cl)
{
    uint32_t first_data = RSV + NFAT * FATSZ;
    return (first_data + (cl - 2) * SPC) * BPS;
}

static void fat_set(uint8_t *fat, uint32_t cl, uint32_t v)
{
    wr32(fat + cl * 4, v);
}

static void put_dirent(uint8_t *ent, const char name[11], uint32_t cl, uint32_t size, uint8_t attr)
{
    memset(ent, 0, 32);
    memcpy(ent, name, 11);
    ent[11] = attr;
    wr16(ent + 26, (uint16_t)(cl & 0xFFFF));
    wr16(ent + 20, (uint16_t)(cl >> 16));
    wr32(ent + 28, size);
}

int main(int argc, char **argv)
{
    const char *path;
    int stale = 0, empty = 0;
    FILE *fp;
    uint8_t *img;
    uint8_t *fat;
    uint8_t *root;
    uint32_t serial = 0x1234ABCD;
    size_t imgsz = (size_t)TOTSEC * BPS;
    uint32_t gap_clus[] = {8, 10, 12, 14, 16};
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: mkfat32 [--stale|--empty] IMAGE\n");
        return 1;
    }
    path = argv[argc - 1];
    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--stale") == 0)
            stale = 1;
        else if (strcmp(argv[i], "--empty") == 0)
            empty = 1;
    }

    img = calloc(1, imgsz);
    if (!img)
        return 1;

    /* Boot sector */
    img[0] = 0xEB;
    img[1] = 0x58;
    img[2] = 0x90;
    memcpy(img + 3, "MSDOS5.0", 8);
    wr16(img + 11, BPS);
    img[13] = SPC;
    wr16(img + 14, RSV);
    img[16] = NFAT;
    wr16(img + 17, 0);
    wr16(img + 19, 0);
    img[21] = 0xF8;
    wr16(img + 22, 0);
    wr16(img + 24, 63);
    wr16(img + 26, 255);
    wr32(img + 32, TOTSEC);
    wr32(img + 36, FATSZ);
    wr16(img + 40, 0);
    wr16(img + 42, 0);
    wr32(img + 44, ROOT);
    wr16(img + 48, 1);
    wr16(img + 50, 6);
    img[64] = 0x80;
    img[66] = 0x29;
    wr32(img + 67, serial);
    memcpy(img + 71, "TESTVOL    ", 11);
    memcpy(img + 82, "FAT32   ", 8);
    img[510] = 0x55;
    img[511] = 0xAA;

    /* Backup boot */
    memcpy(img + 6 * BPS, img, BPS);

    /* FSInfo */
    wr32(img + BPS, 0x41615252);
    wr32(img + BPS + 0x1E4, 0x61417272);
    wr32(img + BPS + 0x1E8, 0xFFFFFFFF);
    wr32(img + BPS + 0x1EC, 0xFFFFFFFF);
    wr32(img + BPS + 0x1FC, 0xAA550000);

    fat = img + RSV * BPS;
    fat_set(fat, 0, 0x0FFFFFF8);
    fat_set(fat, 1, 0x0FFFFFFF);
    fat_set(fat, ROOT, 0x0FFFFFFF);

    root = img + cl_off(ROOT);

    if (!empty) {
        uint8_t filler[11] = "FILLER  BIN";
        uint8_t gapn[11] = "GAP     BIN";
        uint32_t gsz = 5 * CLUSZ;

        fat_set(fat, 3, 4);
        fat_set(fat, 4, 5);
        fat_set(fat, 5, 6);
        fat_set(fat, 6, 0x0FFFFFFF);
        for (i = 0; i < 4; i++) {
            memset(img + cl_off(3 + i), 'F', CLUSZ);
        }
        put_dirent(root, (char *)filler, 3, 4 * CLUSZ, 0x20);

        for (i = 0; i < 5; i++) {
            uint32_t cl = gap_clus[i];
            uint32_t next = (i + 1 < 5) ? gap_clus[i + 1] : 0x0FFFFFFF;
            uint8_t *p = img + cl_off(cl);
            uint32_t b;
            fat_set(fat, cl, next);
            for (b = 0; b < CLUSZ; b++)
                p[b] = (uint8_t)((i * CLUSZ + b) & 0xFF);
        }
        put_dirent(root + 32, (char *)gapn, stale ? 20 : 8, gsz, 0x20);

        if (stale) {
            /* Data already at 20-24, dirent points there, old clusters still allocated. */
            for (i = 0; i < 5; i++) {
                memcpy(img + cl_off(20 + i), img + cl_off(gap_clus[i]), CLUSZ);
                fat_set(fat, 20 + i, (i + 1 < 5) ? 20 + i + 1 : 0x0FFFFFFF);
            }
        }
    }

    /* FAT copy 2 */
    memcpy(img + (RSV + FATSZ) * BPS, fat, FATSZ * BPS);

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
    printf("wrote %s (%s)\n", path, empty ? "empty" : (stale ? "stale-journal layout" : "fragmented"));
    return 0;
}
