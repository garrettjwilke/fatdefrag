#include "../include/fatdefrag.h"
#include "device.h"
#include "fs.h"
#include "util.h"

#include <stdio.h>

int cmd_defrag(const char *device, const char *file, int dry_run, int verbose)
{
    Device *dev = NULL;
    Volume *vol = NULL;
    int rc = -1;

    if (!dry_run)
        warn_backup();

    if (dev_open(device, dry_run ? 0 : 1, &dev) != 0)
        return 1;
    if (dev_is_mounted(dev)) {
        fprintf(stderr, "%s appears mounted; unmount it first (diskutil unmount / umount)\n",
                device);
        goto out;
    }
    if (vol_open(dev, &vol) != 0)
        goto out;
    if (!dry_run && vol_recover(vol) != 0) {
        fprintf(stderr, "journal recovery failed\n");
        goto out;
    }
    if (vol_defrag(vol, file, dry_run, verbose) != 0)
        goto out;
    rc = 0;
out:
    vol_close(vol);
    dev_close(dev);
    return rc == 0 ? 0 : 1;
}
