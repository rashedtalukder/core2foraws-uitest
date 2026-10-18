/*
 * SPDX-FileCopyrightText: 2022 Rashed Talukder
 * SPDX-License-Identifier: MIT
 *
 * Internal declarations for core2foraws-uitest compilation units.
 */

#pragma once

#include "sdkconfig.h"
#include "uitest.h"

#ifdef CONFIG_UITEST_ENABLED

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "lvgl.h"

#define RESP_PREFIX CONFIG_UITEST_CMD_PREFIX
#define READ_PERIOD CONFIG_UITEST_READ_PERIOD_MS

/* One injected pointer sample consumed per LVGL indev read. */
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t pressed; /* 1 = LV_INDEV_STATE_PRESSED, 0 = RELEASED */
} inject_sample_t;

extern lv_indev_t          *s_indev;
extern QueueHandle_t        s_sample_q;
extern SemaphoreHandle_t    s_inject_lock;
extern _Atomic TaskHandle_t s_listener_task;
extern int16_t              s_last_x;
extern int16_t              s_last_y;
extern atomic_bool          s_release_pending;
extern atomic_bool          s_ready;
extern atomic_bool          s_stopping;
extern atomic_bool          s_listener_running;

/* uitest_indev.c */
void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
esp_err_t wait_drained(void);

/* uitest_registry.c */
void cmd_dump(void);
void cmd_get(const char *id);
esp_err_t click_id(const char *id);

/* uitest_cmd.c */
void reply_ok_v(const char *fmt, va_list ap);
void reply_ok(const char *fmt, ...);
void reply_err(const char *msg);
void reply_command_error(const char *command, esp_err_t err);
void listener_task(void *arg);

#endif /* CONFIG_UITEST_ENABLED */
