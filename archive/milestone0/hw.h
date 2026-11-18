/*
 * PortalPlayer PP5022 / iPod Video (5th gen) hardware registers.
 *
 * Addresses are hardware facts, cross-checked against the Rockbox source in
 * reference/rockbox (firmware/export/pp5020.h, target/arm/ipod/video/lcd-video.c,
 * target/arm/ipod/button-clickwheel.c, target/arm/ipod/backlight-nano_video.c,
 * target/arm/pp/system-pp502x.c, bootloader/ipod.c). No Rockbox code is copied.
 */
#pragma once
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))

/* Which core is running: reads 0x55 on the CPU, 0xAA on the COP. */
#define PROC_ID          (*(volatile uint8_t *)0x60000000)
#define PROC_ID_CPU      0x55

#define CPU_INT_DIS      REG32(0x60004028)
#define COP_INT_DIS      REG32(0x60004038)
#define CPU_CTL          REG32(0x60007000)
#define COP_CTL          REG32(0x60007004)
#define PROC_SLEEP       0x80000000

/* Free-running microsecond counter. */
#define USEC_TIMER       REG32(0x60005010)

/* Device reset / enable. Setting DEV_SYSTEM in DEV_RS reboots the iPod. */
#define DEV_RS           REG32(0x60006004)
#define DEV_SYSTEM       0x00000004
#define DEV_INIT1        REG32(0x70000010)
#define INIT_BUTTONS     0x00040000

/* GPIO ports. Each register has an atomic set/clear mirror 0x800 above it:
 * writing (mask << 8) | mask sets the bits, (mask << 8) clears them. */
#define GPIOA_ENABLE     REG32(0x6000d000)
#define GPIOB_ENABLE     REG32(0x6000d004)
#define GPIOD_ENABLE     REG32(0x6000d00c)
#define GPIOA_OUTPUT_EN  REG32(0x6000d010)
#define GPIOB_OUTPUT_EN  REG32(0x6000d014)
#define GPIOD_OUTPUT_EN  REG32(0x6000d01c)
#define GPIOB_OUTPUT_VAL REG32(0x6000d024)
#define GPIOD_OUTPUT_VAL REG32(0x6000d02c)
#define GPIOA_INPUT_VAL  REG32(0x6000d030)
#define GPIOL_ENABLE     REG32(0x6000d10c)
#define GPIOL_OUTPUT_EN  REG32(0x6000d11c)
#define GPIOL_OUTPUT_VAL REG32(0x6000d12c)
#define GPIO_SET(reg, mask)   (*(&(reg) + 0x800 / 4) = ((uint32_t)(mask) << 8) | (uint32_t)(mask))
#define GPIO_CLEAR(reg, mask) (*(&(reg) + 0x800 / 4) = ((uint32_t)(mask) << 8))

#define HOLD_BIT         0x20      /* GPIO A bit 5: HOLD switch, low = on */
#define BACKLIGHT_B      0x08      /* GPIO B bit 3: backlight */
#define BACKLIGHT_D      0x80      /* GPIO D bit 7: backlight */
#define BACKLIGHT_LED_L  0x80      /* GPIO L bit 7: backlight LED */

/* GPO32: bit 14 set = the BCM2722 video chip is powered (and, after the
 * bootloader, already initialised). */
#define GPO32_VAL        REG32(0x70000080)
#define GPO32_BCM_POWER  0x4000

/* Writing this to on-chip RAM and rebooting makes Apple's ROM start in disk mode
 * (PP5022 address/string from bootloader/ipod.c). */
#define DISKMODE_MAGIC_ADDR 0x4001ff00

/*
 * Broadcom BCM2722 video processor. The LCD is behind it: pixels are written
 * into its memory and an "update" command makes it refresh the panel.
 * Low address bits aren't decoded, so sequential 32-bit writes to the data port
 * all land in the same FIFO.
 */
#define BCM_DATA32       REG32(0x30000000)
#define BCM_WR_ADDR32    REG32(0x30010000)
#define BCM_RD_ADDR      REG16(0x30020000)
#define BCM_RD_ADDR32    REG32(0x30020000)
#define BCM_CONTROL      REG16(0x30030000)
#define BCM_CONTROL_KICK 0x31      /* write after a command so the BCM executes it */

#define BCMA_COMMAND     0x1F8     /* write a command here */
#define BCMA_CMDPARAM    0xE0000   /* framebuffer: 320x240 RGB565, row-major */
#define BCM_CMD(x)       ((~((uint32_t)(x)) << 16) | ((uint32_t)(x)))
#define BCMCMD_LCD_UPDATE BCM_CMD(0)

#define LCD_WIDTH        320
#define LCD_HEIGHT       240
