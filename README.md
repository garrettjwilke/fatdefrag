# fatdefrag

Defragment **FAT32** and **exFAT** volumes on Linux and macOS. Works on unmounted partitions or raw filesystem image files.

This is an original C implementation matching the documented CLI (`check`, `defrag`, `copy`). It does not use the old Linux-only binary.

**Backup your data before any write operation.** Run `fsck` first. The volume must be unmounted.

## Build

```bash
make
```

Produces `./fatdefrag` for the host OS (`uname`). POSIX C11, no extra libraries.

```bash
make test
```

FAT32 and exFAT tests are self-contained (no `mkfs` required).

## Usage

```bash
# Analyze entire filesystem
./fatdefrag check /dev/sdX1          # Linux
./fatdefrag check /dev/rdisk2s1      # macOS (raw disk)
./fatdefrag check disk.img           # image file

./fatdefrag check DEVICE -v
./fatdefrag check DEVICE -f /path/to/file.bin

# Defragment (dry-run first)
./fatdefrag defrag DEVICE -n
./fatdefrag defrag DEVICE
./fatdefrag defrag DEVICE -f /path/to/file.bin

# Copy a host file onto the volume as a contiguous file
./fatdefrag copy ./source.bin DEVICE:/dest.bin -n
./fatdefrag copy ./source.bin DEVICE:/dest.bin
```

| Option | Meaning |
|--------|---------|
| `-v, --verbose` | List files / extra progress |
| `-f, --file PATH` | One file inside the volume |
| `-n, --dry-run` | No writes |
| `-h, --help` | Help |

On Linux, use `sudo` or the `disk` group for real devices. On macOS, prefer `/dev/rdiskNsM` and `diskutil unmount` (not eject) before running.

Defrag moves a fragmented file into a contiguous free hole. If no hole large enough exists, that file is skipped (this version does not compact the whole volume).

Interrupted defrags are journaled on the volume and completed the next time you run `defrag`.

## License

MIT (see [LICENSE](LICENSE)).
