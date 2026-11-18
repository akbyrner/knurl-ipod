/* In-place writes into preallocated files (device/store.c). */
#pragma once
#include <stdint.h>
#include "storage/fat.h"

/* Open a file the computer created at its full size; at least min_size bytes. 0 = ok. */
int store_open(const char *path, fat_file *f, uint32_t min_size);
/* Whole sectors: pos and n are multiples of 512, buf 4-aligned. 0 = ok. */
int store_write(fat_file *f, uint32_t pos, const void *buf, uint32_t n);
int store_read(fat_file *f, uint32_t pos, void *buf, uint32_t n);

/* fwinstall.c */
void boot_stamp_read(void);               /* first thing in main(): who started us */
void fw_maintenance(void);                /* after the disk is mounted: loader install */

/* power.c: the battery log, /.knurl/battery.log */
void power_log_start(void);               /* after the disk is mounted */
void power_log_flush(void);               /* before disk mode or a restart */
void power_log_event(int code);           /* LOG_OFF_* below, then flushed */
enum { LOG_OFF_BUTTON = 1, LOG_OFF_IDLE = 2, LOG_OFF_BATTERY = 3 };
