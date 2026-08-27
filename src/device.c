#define _FILE_OFFSET_BITS 64

#include "device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/disk.h>
#include <sys/mount.h>
#endif

#ifdef __linux__
#include <linux/fs.h>
#include <sys/sysmacros.h>
#endif

#include <sys/ioctl.h>

struct Device {
    int fd;
    int writable;
    int is_block;
    uint64_t size;
    char path[1024];
    char real[1024];
    dev_t rdev;
};

static int read_all(int fd, uint64_t off, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EINVAL;
            return -1;
        }
        p += n;
        off += (uint64_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_all(int fd, uint64_t off, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = pwrite(fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += n;
        off += (uint64_t)n;
        len -= (size_t)n;
    }
    return 0;
}

#ifdef __linux__
static int linux_loop_backs_path(const char *real)
{
    /* Check /sys/block/loop*/loop/backing_file against realpath. */
    for (int i = 0; i < 256; i++) {
        char p[128], back[1024];
        FILE *f;
        size_t n;
        snprintf(p, sizeof p, "/sys/block/loop%d/loop/backing_file", i);
        f = fopen(p, "r");
        if (!f)
            continue;
        if (!fgets(back, sizeof back, f)) {
            fclose(f);
            continue;
        }
        fclose(f);
        n = strlen(back);
        while (n > 0 && (back[n - 1] == '\n' || back[n - 1] == '\r'))
            back[--n] = 0;
        if (n && strcmp(back, real) == 0)
            return 1;
    }
    return 0;
}

static int linux_blk_mounted(dev_t rdev)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[2048];
    unsigned maj = major(rdev), minr = minor(rdev);
    if (!f)
        f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        unsigned m = 0, n = 0;
        /* mountinfo: ... major:minor ... */
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        /* scan all maj:min tokens */
        for (char *p = line; *p; p++) {
            if (p > line && p[-1] != ' ' && p != line)
                continue;
            if (sscanf(p, "%u:%u", &m, &n) == 2 && m == maj && n == minr) {
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}
#endif

#ifdef __APPLE__
static void strip_rdisk(const char *in, char *out, size_t n)
{
    const char *base = strrchr(in, '/');
    base = base ? base + 1 : in;
    if (strncmp(base, "rdisk", 5) == 0)
        snprintf(out, n, "/dev/%s", base + 1); /* rdisk -> disk */
    else
        snprintf(out, n, "%s", in);
}

static int darwin_mounted(const Device *d)
{
    struct statfs *mnts = NULL;
    int n = getmntinfo(&mnts, MNT_NOWAIT);
    char alt[1024];
    strip_rdisk(d->path, alt, sizeof alt);
    if (n <= 0)
        return 0;
    for (int i = 0; i < n; i++) {
        const char *from = mnts[i].f_mntfromname;
        if (strcmp(from, d->path) == 0 || strcmp(from, alt) == 0)
            return 1;
        if (d->is_block) {
            struct stat st;
            if (stat(from, &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == d->rdev)
                return 1;
        }
    }
    return 0;
}
#endif

int dev_open(const char *path, int writable, Device **out)
{
    Device *d;
    struct stat st;
    int flags = writable ? O_RDWR : O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif

    d = calloc(1, sizeof *d);
    if (!d)
        return -1;
    snprintf(d->path, sizeof d->path, "%s", path);
    if (!realpath(path, d->real))
        snprintf(d->real, sizeof d->real, "%s", path);

    d->fd = open(path, flags);
    if (d->fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        free(d);
        return -1;
    }
    if (fstat(d->fd, &st) != 0) {
        close(d->fd);
        free(d);
        return -1;
    }
    d->writable = writable;
    d->is_block = S_ISBLK(st.st_mode) ? 1 : 0;
    d->rdev = st.st_rdev;

    if (S_ISREG(st.st_mode)) {
        d->size = (uint64_t)st.st_size;
    } else if (d->is_block) {
#ifdef __APPLE__
        uint32_t bsz = 0;
        uint64_t blocks = 0;
        if (ioctl(d->fd, DKIOCGETBLOCKSIZE, &bsz) != 0)
            bsz = 512;
        if (ioctl(d->fd, DKIOCGETBLOCKCOUNT, &blocks) != 0) {
            fprintf(stderr, "cannot get size of %s\n", path);
            close(d->fd);
            free(d);
            return -1;
        }
        d->size = blocks * (uint64_t)bsz;
#elif defined(__linux__)
        uint64_t bytes = 0;
        if (ioctl(d->fd, BLKGETSIZE64, &bytes) != 0) {
            fprintf(stderr, "cannot get size of %s\n", path);
            close(d->fd);
            free(d);
            return -1;
        }
        d->size = bytes;
#else
        fprintf(stderr, "block devices not supported on this OS\n");
        close(d->fd);
        free(d);
        return -1;
#endif
    } else {
        fprintf(stderr, "%s: not a block device or regular file\n", path);
        close(d->fd);
        free(d);
        return -1;
    }

    *out = d;
    return 0;
}

void dev_close(Device *d)
{
    if (!d)
        return;
    if (d->fd >= 0)
        close(d->fd);
    free(d);
}

const char *dev_path(const Device *d) { return d->path; }
int dev_writable(const Device *d) { return d->writable; }
int dev_is_block(const Device *d) { return d->is_block; }
uint64_t dev_size(const Device *d) { return d->size; }

int dev_read(Device *d, uint64_t off, void *buf, size_t len)
{
    if (off + len > d->size) {
        fprintf(stderr, "read past end of device (off=%llu len=%zu size=%llu)\n",
                (unsigned long long)off, len, (unsigned long long)d->size);
        return -1;
    }
    if (read_all(d->fd, off, buf, len) != 0) {
        fprintf(stderr, "read failed at %llu: %s\n", (unsigned long long)off, strerror(errno));
        return -1;
    }
    return 0;
}

int dev_write(Device *d, uint64_t off, const void *buf, size_t len)
{
    if (!d->writable) {
        fprintf(stderr, "device opened read-only\n");
        return -1;
    }
    if (off + len > d->size) {
        fprintf(stderr, "write past end of device\n");
        return -1;
    }
    if (write_all(d->fd, off, buf, len) != 0) {
        fprintf(stderr, "write failed at %llu: %s\n", (unsigned long long)off, strerror(errno));
        return -1;
    }
    return 0;
}

int dev_sync(Device *d)
{
    if (!d->writable)
        return 0;
    if (fsync(d->fd) != 0) {
        fprintf(stderr, "fsync: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int dev_is_mounted(const Device *d)
{
#ifdef __linux__
    if (d->is_block)
        return linux_blk_mounted(d->rdev);
    if (linux_loop_backs_path(d->real)) {
        /* backing file of a loop device — check if that loop is mounted */
        for (int i = 0; i < 256; i++) {
            char p[128], back[1024];
            FILE *f;
            struct stat st;
            char loopdev[64];
            snprintf(p, sizeof p, "/sys/block/loop%d/loop/backing_file", i);
            f = fopen(p, "r");
            if (!f)
                continue;
            if (!fgets(back, sizeof back, f)) {
                fclose(f);
                continue;
            }
            fclose(f);
            size_t n = strlen(back);
            while (n > 0 && (back[n - 1] == '\n' || back[n - 1] == '\r'))
                back[--n] = 0;
            if (strcmp(back, d->real) != 0)
                continue;
            snprintf(loopdev, sizeof loopdev, "/dev/loop%d", i);
            if (stat(loopdev, &st) == 0 && linux_blk_mounted(st.st_rdev))
                return 1;
        }
    }
    return 0;
#elif defined(__APPLE__)
    return darwin_mounted(d);
#else
    (void)d;
    return 0;
#endif
}
