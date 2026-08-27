#include "../include/fatdefrag.h"
#include "device.h"
#include "fs.h"
#include "util.h"

#include <stdio.h>

int cmd_check(const char *device, const char *file, int verbose)
{
    Device *dev = NULL;
    Volume *vol = NULL;
    CheckResult res;
    int rc = -1;

    if (dev_open(device, 0, &dev) != 0)
        return 1;
    if (dev_is_block(dev) && dev_is_mounted(dev)) {
        fprintf(stderr, "%s is mounted; unmount it first\n", device);
        goto out;
    }
    if (vol_open(dev, &vol) != 0)
        goto out;
    if (vol_journal_dirty(vol))
        fprintf(stderr, "warning: incomplete defrag journal present (run defrag to recover)\n");
    if (vol_check(vol, file, verbose, &res) != 0)
        goto out;

    printf("%s: %s\n", device, res.kind);
    printf("volume serial: %04X-%04X\n", (res.serial >> 16) & 0xFFFF, res.serial & 0xFFFF);
    printf("cluster size: %u bytes\n", res.cluster_bytes);
    printf("files: %llu\n", (unsigned long long)res.files);
    printf("fragmented: %llu", (unsigned long long)res.fragmented);
    if (res.files)
        printf(" (%.1f%%)", 100.0 * (double)res.fragmented / (double)res.files);
    printf("\n");
    printf("worst extents: %u\n", res.worst_extents);
    rc = 0;
out:
    vol_close(vol);
    dev_close(dev);
    return rc == 0 ? 0 : 1;
}
