#include "journal.h"
#include "util.h"
#include "device.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path;
    Device *dev;
    uint8_t boot[512];
    Journal j;
    uint32_t serial, bps, reserved;

    if (argc < 2) {
        fprintf(stderr, "usage: plantjnl IMAGE\n");
        return 1;
    }
    path = argv[1];
    if (dev_open(path, 1, &dev) != 0)
        return 1;
    if (dev_read(dev, 0, boot, 512) != 0) {
        dev_close(dev);
        return 1;
    }
    bps = rd_le16(boot + 11);
    reserved = rd_le16(boot + 14);
    serial = rd_le32(boot + 67);
    memset(&j, 0, sizeof j);
    memcpy(j.magic, JNL_MAGIC, 8);
    j.serial = serial;
    j.phase = JNL_DIRENT_UPDATED;
    j.fs_kind = 32;
    j.old_first = 8;
    j.new_first = 20;
    j.nclusters = 5;
    /* root is cluster 2; GAP is the second dirent (+32) */
    j.dirent_off = ((uint64_t)(reserved + 2u * 64u) * bps) + 32;
    if (journal_write(dev, reserved - 1, bps, &j) != 0) {
        fprintf(stderr, "journal write failed\n");
        dev_close(dev);
        return 1;
    }
    printf("planted journal on %s (serial %08X lba %u)\n", path, serial, reserved - 1);
    dev_close(dev);
    return 0;
}
