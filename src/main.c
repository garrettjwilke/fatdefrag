#include "../include/fatdefrag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *fp)
{
    fprintf(fp,
            "fatdefrag — defragment FAT32 and exFAT volumes (Linux and macOS)\n"
            "\n"
            "Usage:\n"
            "  fatdefrag check  DEVICE [-v] [-f PATH]\n"
            "  fatdefrag defrag DEVICE [-n] [-v] [-f PATH]\n"
            "  fatdefrag copy   SOURCE DEVICE:DEST [-n] [-v]\n"
            "\n"
            "Options:\n"
            "  -v, --verbose     Show detailed output\n"
            "  -f, --file PATH   Operate on one file inside the volume\n"
            "  -n, --dry-run     Don't make changes\n"
            "  -h, --help        Show help\n"
            "\n"
            "DEVICE is an unmounted partition (Linux /dev/sdXN, macOS /dev/rdiskNsM)\n"
            "or a raw filesystem image file.\n"
            "\n"
            "WARNING: Backup your data before defrag or copy.\n");
}

int main(int argc, char **argv)
{
    const char *cmd;
    const char *file = NULL;
    int verbose = 0, dry = 0;
    const char *pos[4];
    int npos = 0;

    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0) {
            dry = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "-f requires a path\n");
                return 1;
            }
            file = argv[++i];
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(stderr);
            return 1;
        } else if (npos < 4) {
            pos[npos++] = argv[i];
        } else {
            fprintf(stderr, "too many arguments\n");
            return 1;
        }
    }

    if (strcmp(cmd, "check") == 0) {
        if (npos != 1) {
            fprintf(stderr, "usage: fatdefrag check DEVICE [-v] [-f PATH]\n");
            return 1;
        }
        return cmd_check(pos[0], file, verbose);
    }
    if (strcmp(cmd, "defrag") == 0) {
        if (npos != 1) {
            fprintf(stderr, "usage: fatdefrag defrag DEVICE [-n] [-v] [-f PATH]\n");
            return 1;
        }
        return cmd_defrag(pos[0], file, dry, verbose);
    }
    if (strcmp(cmd, "copy") == 0) {
        if (npos != 2) {
            fprintf(stderr, "usage: fatdefrag copy SOURCE DEVICE:DEST [-n] [-v]\n");
            return 1;
        }
        if (file) {
            fprintf(stderr, "copy does not take -f; use DEVICE:DEST\n");
            return 1;
        }
        return cmd_copy(pos[0], pos[1], dry, verbose);
    }

    fprintf(stderr, "unknown command: %s\n", cmd);
    usage(stderr);
    return 1;
}
