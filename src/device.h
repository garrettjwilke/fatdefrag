#ifndef FATDEFRAG_DEVICE_H
#define FATDEFRAG_DEVICE_H

#include <stdint.h>
#include <stddef.h>

typedef struct Device Device;

int dev_open(const char *path, int writable, Device **out);
void dev_close(Device *d);

const char *dev_path(const Device *d);
int dev_writable(const Device *d);
int dev_is_block(const Device *d);
uint64_t dev_size(const Device *d);

int dev_read(Device *d, uint64_t off, void *buf, size_t len);
int dev_write(Device *d, uint64_t off, const void *buf, size_t len);
int dev_sync(Device *d);

/* Returns 1 if a real block device is currently mounted (unsafe to mutate). */
int dev_is_mounted(const Device *d);

#endif
