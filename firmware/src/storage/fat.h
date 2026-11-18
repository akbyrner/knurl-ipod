/*
 * Read-only FAT32: just enough to open a file by path and read it. Long file names are
 * supported (PodManager writes them). The file system itself is never written, by design;
 * the most Knurl does is overwrite sectors inside a file the computer created at its full
 * size (device/store.c), using fat_file_lba to find them.
 */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t first_cluster;
    uint32_t size;                /* bytes (0xFFFFFFFF for a directory: read to the end of its chain) */
    uint32_t pos;
    uint32_t cluster;             /* cluster holding pos ... */
    uint32_t cluster_idx;         /* ... and its index in the chain */
} fat_file;

int         fat_mount(void);                       /* 0 = ok */
int         fat_mbr_units(void);                   /* partition-table unit: 1 or 4 (x512 bytes) */
const char *fat_status(void);                      /* what happened, for the diagnostics screen */
uint32_t    fat_cluster_bytes(void);
int         fat_open(const char *path, fat_file *f);   /* 0 = ok; path from the root, '/'-separated */
int         fat_read(fat_file *f, void *buf, uint32_t n); /* bytes read (short at end), -1 on error */
int         fat_seek(fat_file *f, uint32_t pos);
/* The disk sector holding byte pos of the file, and how many sectors follow it in the same
 * cluster (*run, counting that one). 0 = ok. */
int         fat_file_lba(fat_file *f, uint32_t pos, uint32_t *lba, uint32_t *run);
void        fat_forget(void);                      /* drop the sector caches (after a write) */
