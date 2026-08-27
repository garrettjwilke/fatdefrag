#include "../include/fatdefrag.h"
#include "device.h"
#include "fs.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_copy(const char *source, const char *dest_spec, int dry_run, int verbose)
{
    Device *dev = NULL;
    Volume *vol = NULL;
    char *spec, *colon, *device, *volpath;
    int rc = -1;

    spec = strdup(dest_spec);
    if (!spec)
        return 1;
    colon = strrchr(spec, ':');
    if (!colon || colon == spec || !colon[1]) {
        fprintf(stderr, "destination must be DEVICE:PATH (e.g. /dev/rdisk2s1:/file.bin or image.img:/file.bin)\n");
        free(spec);
        return 1;
    }
    *colon = 0;
    device = spec;
    volpath = colon + 1;

    if (!dry_run)
        warn_backup();

    if (dev_open(device, dry_run ? 0 : 1, &dev) != 0) {
        free(spec);
        return 1;
    }
    if (dev_is_mounted(dev)) {
        fprintf(stderr, "%s appears mounted; unmount it first\n", device);
        goto out;
    }
    if (vol_open(dev, &vol) != 0)
        goto out;
    if (!dry_run && vol_recover(vol) != 0) {
        fprintf(stderr, "journal recovery failed\n");
        goto out;
    }
    if (vol_copy(vol, source, volpath, dry_run, verbose) != 0)
        goto out;
    rc = 0;
out:
    vol_close(vol);
    dev_close(dev);
    free(spec);
    return rc == 0 ? 0 : 1;
}
