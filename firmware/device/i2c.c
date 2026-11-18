/*
 * I2C: the PP5022's controller, used for the WM8758 codec (0x1a) and the PCF50605 power
 * chip (0x08). Transfers of 1-4 bytes; every wait is bounded. Only the CPU calls this.
 * Register behaviour as in Rockbox's i2c-pp.c (reference/rockbox).
 */
#include "hw.h"
#include "dev.h"

static int i2c_wait(void)
{
    uint32_t t = USEC_TIMER;
    while (I2C_STATUS & I2C_BUSY)
        if (USEC_TIMER - t > 20000) return -1;
    return 0;
}

void i2c_init(void)
{
    DEV_EN |= DEV_I2C;
    DEV_RS |= DEV_I2C;
    DEV_RS &= ~DEV_I2C;
    I2C_SETUP = 0;
    I2C_SETUP = 0x80;
}

int i2c_write(int dev, const uint8_t *data, int len)
{
    if (len < 1 || len > 4 || i2c_wait()) return -1;
    I2C_ADDR = (uint8_t)(dev << 1);
    I2C_CTRL &= ~0x20;                         /* write */
    for (int i = 0; i < len; i++) I2C_DATA(i) = data[i];
    I2C_CTRL = (uint8_t)((I2C_CTRL & ~0x6) | ((len - 1) << 1));
    I2C_CTRL |= I2C_SEND;
    return i2c_wait();
}

int i2c_read(int dev, int reg, uint8_t *data, int len)
{
    uint8_t r = (uint8_t)reg;
    if (len < 1 || len > 4 || i2c_write(dev, &r, 1)) return -1;
    I2C_ADDR = (uint8_t)((dev << 1) | 1);
    I2C_CTRL |= 0x20;                          /* read */
    I2C_CTRL = (uint8_t)((I2C_CTRL & ~0x6) | ((len - 1) << 1));
    I2C_CTRL |= I2C_SEND;
    if (i2c_wait()) return -1;
    for (int i = 0; i < len; i++) data[i] = I2C_DATA(i);
    return 0;
}
