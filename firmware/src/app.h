#pragma once
#include <stdint.h>
#include "platform.h"

/* The whole user interface. The platform's main loop calls app_step() with what happened
 * and the time; the app redraws what changed and calls platform_present() for just that
 * area. Returns 1 while an animation is running (call again soon), 0 when idle. */
void app_init(uint32_t now_ms);
int  app_step(const input_t *in, uint32_t now_ms);
