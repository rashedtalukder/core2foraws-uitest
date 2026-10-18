/*
 * SPDX-FileCopyrightText: 2022 Rashed Talukder
 * SPDX-License-Identifier: MIT
 *
 * Internal synthetic LVGL pointer input device and gesture sample queue.
 */

#pragma once

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
#include "esp_lvgl_port.h"

#define READ_PERIOD CONFIG_UITEST_READ_PERIOD_MS

/* One injected pointer sample consumed per LVGL indev read. */
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t pressed;    /* 1 = LV_INDEV_STATE_PRESSED, 0 = RELEASED */
} inject_sample_t;

static lv_indev_t        *s_indev;
static QueueHandle_t      s_sample_q;
static SemaphoreHandle_t  s_inject_lock;
static _Atomic TaskHandle_t s_listener_task;
static int16_t            s_last_x;
static int16_t            s_last_y;
static atomic_bool        s_release_pending;
static atomic_bool        s_ready;

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    inject_sample_t s;
    TaskHandle_t listener = atomic_load(&s_listener_task);

    if (atomic_exchange(&s_release_pending, false)) {
        lv_indev_reset(indev, NULL);
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        if (listener != NULL) xTaskNotifyGive(listener);
        return;
    }

    if (s_sample_q != NULL &&
        xQueueReceive(s_sample_q, &s, 0) == pdTRUE) {
        s_last_x = s.x;
        s_last_y = s.y;
        data->point.x = s.x;
        data->point.y = s.y;
        data->state = s.pressed ? LV_INDEV_STATE_PRESSED
                                : LV_INDEV_STATE_RELEASED;
        if (uxQueueMessagesWaiting(s_sample_q) == 0 && listener != NULL) {
            xTaskNotifyGive(listener);
        }
    } else {
        /* Idle: hold last position, report released. */
        data->point.x = s_last_x;
        data->point.y = s_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    /* Leave continue_reading false so one sample is consumed per read
     * period — this spreads a gesture over wall-clock time, which is what
     * LVGL's scroll/gesture velocity detection expects. */
}

/* ── Bounds ─────────────────────────────────────────────────────── */

static esp_err_t get_display_size(int32_t *width, int32_t *height)
{
    if (width == NULL || height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lvgl_port_lock(CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_display_t *display = lv_display_get_default();
    if (display == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    *width = lv_display_get_horizontal_resolution(display);
    *height = lv_display_get_vertical_resolution(display);
    lvgl_port_unlock();
    return ESP_OK;
}

static esp_err_t validate_point(int16_t x, int16_t y)
{
    int32_t width;
    int32_t height;
    esp_err_t err = get_display_size(&width, &height);
    if (err != ESP_OK) {
        return err;
    }
    return x >= 0 && y >= 0 && x < width && y < height
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t validate_two_points(int16_t x0, int16_t y0,
                                     int16_t x1, int16_t y1)
{
    int32_t width;
    int32_t height;
    esp_err_t err = get_display_size(&width, &height);
    if (err != ESP_OK) return err;
    bool valid = x0 >= 0 && y0 >= 0 && x0 < width && y0 < height &&
                 x1 >= 0 && y1 >= 0 && x1 < width && y1 < height;
    return valid ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t reserve_samples(size_t sample_count)
{
    if (!atomic_load(&s_ready)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count == 0 || sample_count > CONFIG_UITEST_SAMPLE_QUEUE_DEPTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (xSemaphoreTake(s_inject_lock,
                       pdMS_TO_TICKS(CONFIG_UITEST_DRAIN_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(CONFIG_UITEST_DRAIN_TIMEOUT_MS);
    while (uxQueueSpacesAvailable(s_sample_q) < sample_count) {
        if ((xTaskGetTickCount() - started) >= timeout) {
            xSemaphoreGive(s_inject_lock);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

static esp_err_t enqueue_reserved(int16_t x, int16_t y, bool pressed)
{
    inject_sample_t sample = {
        .x = x,
        .y = y,
        .pressed = pressed ? 1 : 0,
    };
    return xQueueSend(s_sample_q, &sample, 0) == pdTRUE
        ? ESP_OK
        : ESP_FAIL;
}

static void finish_samples(void)
{
    xSemaphoreGive(s_inject_lock);
}

esp_err_t uitest_cancel(void)
{
    if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_inject_lock, pdMS_TO_TICKS(CONFIG_UITEST_DRAIN_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    xQueueReset(s_sample_q);
    atomic_store(&s_release_pending, true);
    xSemaphoreGive(s_inject_lock);
    return ESP_OK;
}

static esp_err_t wait_drained(void)
{
    ulTaskNotifyTake(pdTRUE, 0);
    while (s_sample_q != NULL &&
           (uxQueueMessagesWaiting(s_sample_q) > 0 || atomic_load(&s_release_pending))) {
        if (ulTaskNotifyTake(pdTRUE,
                pdMS_TO_TICKS(CONFIG_UITEST_DRAIN_TIMEOUT_MS)) == 0) {
            uitest_cancel();
            return ESP_ERR_TIMEOUT;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(READ_PERIOD + CONFIG_UITEST_SETTLE_MS));
    return ESP_OK;
}

/* ── Public injection API ───────────────────────────────────────── */

esp_err_t uitest_tap(int16_t x, int16_t y)
{
    esp_err_t err = validate_point(x, y);
    if (err != ESP_OK) {
        return err;
    }
    err = reserve_samples(2);
    if (err != ESP_OK) {
        return err;
    }
    err = enqueue_reserved(x, y, true);
    if (err == ESP_OK) err = enqueue_reserved(x, y, false);
    finish_samples();
    return err;
}

esp_err_t uitest_long_press(int16_t x, int16_t y, uint32_t hold_ms)
{
    esp_err_t err = validate_point(x, y);
    if (err != ESP_OK || hold_ms == 0 || hold_ms > CONFIG_UITEST_MAX_GESTURE_MS) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }
    uint32_t steps = (hold_ms + READ_PERIOD - 1) / READ_PERIOD;
    err = reserve_samples((size_t)steps + 1);
    if (err != ESP_OK) {
        return err;
    }
    for (uint32_t i = 0; i < steps; i++) {
        err = enqueue_reserved(x, y, true);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = enqueue_reserved(x, y, false);
    finish_samples();
    return err;
}

esp_err_t uitest_swipe(int16_t x0, int16_t y0,
                       int16_t x1, int16_t y1,
                       uint32_t duration_ms)
{
    esp_err_t err = validate_two_points(x0, y0, x1, y1);
    if (err != ESP_OK || duration_ms == 0 ||
        duration_ms > CONFIG_UITEST_MAX_GESTURE_MS) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }
    uint32_t intervals = (duration_ms + READ_PERIOD - 1) / READ_PERIOD;
    if (intervals < 2) {
        intervals = 2;
    }
    err = reserve_samples((size_t)intervals + 2);
    if (err != ESP_OK) {
        return err;
    }
    for (uint32_t i = 0; i <= intervals; i++) {
        int32_t x = x0 + (int32_t)(x1 - x0) * (int32_t)i / (int32_t)intervals;
        int32_t y = y0 + (int32_t)(y1 - y0) * (int32_t)i / (int32_t)intervals;
        err = enqueue_reserved((int16_t)x, (int16_t)y, true);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = enqueue_reserved(x1, y1, false);
    finish_samples();
    return err;
}
