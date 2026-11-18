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
#define REG8(a)  (*(volatile uint8_t *)(a))

/* Which core is running: reads 0x55 on the CPU, 0xAA on the COP. */
#define PROC_ID          (*(volatile uint8_t *)0x60000000)
#define PROC_ID_CPU      0x55

#define CPU_INT_EN       REG32(0x60004024)
#define CPU_INT_DIS      REG32(0x60004028)
#define CPU_INT_PRIORITY REG32(0x6000402c)   /* set bit = that source raises FIQ instead of IRQ */
#define CPU_HI_INT_EN    REG32(0x60004124)
#define CPU_HI_INT_DIS   REG32(0x60004128)
#define TIMER1_MASK      (1u << 0)
#define HI_MASK          (1u << 30)     /* the second bank (CPU_HI_INT_*) */
#define WHEEL_HI_MASK    (1u << 8)      /* in the second bank: the click wheel controller */
#define CPU_HI_INT_PRIORITY REG32(0x6000412c)
#define DMA_IRQ_MASK     (1u << 26)
#define COP_INT_DIS      REG32(0x60004038)
#define CPU_CTL          REG32(0x60007000)
#define COP_CTL          REG32(0x60007004)
#define PROC_SLEEP       0x80000000

/* Free-running microsecond counter. */
#define USEC_TIMER       REG32(0x60005010)
#define TIMER1_CFG       REG32(0x60005000)   /* bit 31 enable, bit 30 repeat, low bits = us - 1 */
#define TIMER1_VAL       REG32(0x60005004)   /* reading it acknowledges the interrupt */

/* Device reset / enable. Setting DEV_SYSTEM in DEV_RS reboots the iPod. */
#define DEV_RS           REG32(0x60006004)
#define DEV_SYSTEM       0x00000004
#define DEV_EN           REG32(0x6000600c)
#define DEV_OPTO         0x00010000   /* click wheel controller */
#define DEV_EXTCLOCKS    0x00000002
#define DEV_I2S          0x00000800
#define DEV_I2C          0x00001000
#define DEV_EXTCLOCK_CFG REG32(0x70000018)   /* bits 2-3 clear = external device clock 24 MHz */
#define DEV_INIT1        REG32(0x70000010)
#define DEV_INIT2        REG32(0x70000020)
#define INIT_PLL         0x40000000
#define DEV_TIMING1      REG32(0x70000034)
#define CLOCK_SOURCE     REG32(0x60006020)
#define PLL_CONTROL      REG32(0x60006034)
#define PLL_STATUS       REG32(0x6000603c)
#define PLL_LOCKED       0x80000000

/* Cache: 8 KB, 16-byte lines; with this setup it caches logical 0x00000000-0x03ffffff. */
#define CACHE_PRIORITY   REG32(0x60006044)
#define CACHE_CTL        REG32(0x6000c000)
#define CACHE_MASK       REG32(0xf000f040)
#define CACHE_OPERATION  REG32(0xf000f044)
#define CACHE_CTL_ENABLE 0x0001
#define CACHE_CTL_RUN    0x0002
#define CACHE_CTL_INIT   0x0004
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
#define GPIOA_OUTPUT_VAL REG32(0x6000d020)
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


/* Click wheel controller (from bootloader-era ipodlinux/Rockbox button-clickwheel.c). */
#define WHEEL_CTRL         REG32(0x7000c100)
#define WHEEL_STATUS       REG32(0x7000c104)
#define WHEEL_STATUS_READY 0x04000000   /* a packet is waiting in WHEEL_DATA */
#define WHEEL_DATA         REG32(0x7000c140)

/* I2C controller (the WM8758 codec sits at address 0x1a). */
#define I2C_CTRL           REG8(0x7000c000)
#define I2C_ADDR           REG8(0x7000c004)
#define I2C_DATA(n)        REG8(0x7000c00c + 4 * (n))
#define I2C_STATUS         REG8(0x7000c01c)
#define I2C_SEND           0x80
#define I2C_BUSY           0x40
#define I2C_SETUP          REG32(0x600060a4)

/* I2S controller. With FIFO format LE16_2, one 32-bit write = one stereo frame,
 * left in the low halfword. The codec is the clock master; the PP is slave. */
#define IISCONFIG          REG32(0x70002800)
#define IISFIFO_CFG        REG32(0x7000280c)
#define IISFIFO_WR         REG32(0x70002840)
#define IIS_RESET          (1u << 31)
#define IIS_TXFIFOEN       (1u << 29)
#define IIS_FORMAT_MASK    (3u << 10)    /* 0 = standard I2S */
#define IIS_SIZE_MASK      (3u << 8)     /* 0 = 16 bit */
#define IIS_FIFO_FORMAT_MASK (7u << 4)
#define IIS_FIFO_FORMAT_LE16_2 (7u << 4)
#define IIS_RX_FULL_LVL_12 (3u << 4)
#define IIS_TX_EMPTY_LVL_4 (1u << 0)
#define IIS_RXCLR          (1u << 12)
#define IIS_TXCLR          (1u << 8)
#define IIS_TX_FREE_COUNT  ((IISFIFO_CFG >> 16) & 0x3f)

/* DMA. Channel 0 feeds the I2S FIFO. Addresses given to DMA are physical. */
#define DMA_MASTER_CONTROL REG32(0x6000a000)
#define DMA_MASTER_EN      (1u << 31)
#define DMA_REQ_STATUS     REG32(0x6000a008)
#define DMA_REQ_IIS        2
#define DMA0_CMD           REG32(0x6000b000)
#define DMA0_STATUS        REG32(0x6000b004)   /* reading it acknowledges the interrupt */
#define DMA0_RAM_ADDR      REG32(0x6000b010)
#define DMA0_FLAGS         REG32(0x6000b014)
#define DMA0_PER_ADDR      REG32(0x6000b018)
#define DMA0_INCR          REG32(0x6000b01c)
#define DMA_CMD_WAIT_REQ   (1u << 24)
#define DMA_CMD_SINGLE     (1u << 26)
#define DMA_CMD_RAM_TO_PER (1u << 27)
#define DMA_CMD_INTR       (1u << 30)
#define DMA_CMD_START      (1u << 31)
#define DMA_STATUS_BUSY    (1u << 31)
#define DMA_STATUS_INTR    (1u << 30)
#define DMA_FLAGS_UNK26    (1u << 26)
#define DMA_INCR_RANGE_FIXED (1u << 16)
#define DMA_INCR_WIDTH_32BIT (2u << 28)

/* SDRAM's physical address. Logical 0..32 MB (cached) is remapped onto it by crt0.S;
 * the physical alias itself is never cached, which is what DMA buffers use. */
#define SDRAM_PHYS         0x10000000u
