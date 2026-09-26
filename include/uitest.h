/**
 * @file uitest.h
 * @brief LVGL UI test input and widget registration API.
 *
 * SPDX-FileCopyrightText: 2026 Rashed Talukder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core2 for AWS UI Test Harness
 *
 * A Playwright-style automation layer for LVGL on the device. Registers a
 * synthetic pointer input device and a UART command listener so a host test
 * runner can inject taps, clicks, long-presses and swipes against the real
 * UI, then assert results via the screenshot component.
 *
 * Coordinate space matches the display 1:1 (origin top-left, x right,
 * y down). On the Core2 that is 320 x 240 (landscape), no axis swap/mirror.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the UI test harness.
 *
 * Creates the synthetic LVGL pointer input device and starts the serial
 * command listener task. Call once after the display/LVGL port is up
 * (i.e. after core2foraws_init()).
 *
 * @return ESP_OK on success, ESP_ERR_* otherwise.
 */
esp_err_t uitest_init(void);

/**
 * @brief Stop the listener and remove the synthetic input device.
 *
 * Waits for an in-progress command to finish (bounded by the drain timeout).
 * Returns ESP_ERR_TIMEOUT if the listener is still busy; retry later. Must not
 * be called from a command handler. uitest_init() may be called again after.
 * @return ESP_OK on shutdown or ESP_ERR_TIMEOUT while the listener is busy.
 */
esp_err_t uitest_deinit(void);

/** Cancel queued gestures and reset the pointer on its next LVGL read.
 * @return ESP_OK on success or ESP_ERR_INVALID_STATE if not initialized.
 */
esp_err_t uitest_cancel(void);

/**
 * @brief Associate a stable string id with an LVGL object.
 *
 * Lets the host target the widget by name (`CLICK id`) instead of by
 * pixel coordinates, so tests survive layout changes. The identifier is
 * copied, must be unique, and is removed automatically when the object is
 * deleted.
 *
 * @param obj  The LVGL object to tag. Must not be NULL.
 * @param id   Stable identifier (e.g. "home.wifi_btn").
 * @return ESP_OK, ESP_ERR_NO_MEM if the table is full, ESP_ERR_INVALID_STATE
 *         if the ID belongs to another object, or ESP_ERR_INVALID_ARG.
 */
esp_err_t uitest_register(lv_obj_t *obj, const char *id);

/**
 * @brief Inject a tap (press then release) at a display coordinate.
 * @param x Horizontal display coordinate.
 * @param y Vertical display coordinate.
 * @return ESP_OK, ESP_ERR_INVALID_ARG if out of bounds, or a queue/lock error.
 */
esp_err_t uitest_tap(int16_t x, int16_t y);

/**
 * @brief Inject a press held for at least @p hold_ms then a release.
 *
 * Duration is rounded up to the configured synthetic input read period.
 * @param x Horizontal display coordinate.
 * @param y Vertical display coordinate.
 * @param hold_ms Minimum press duration in milliseconds.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for invalid input, or a queue/lock error.
 */
esp_err_t uitest_long_press(int16_t x, int16_t y, uint32_t hold_ms);

/**
 * @brief Inject a swipe (pressed drag) from (x0,y0) to (x1,y1).
 *
 * The path is interpolated across multiple injected samples so LVGL
 * registers it as a continuous gesture/scroll over at least @p duration_ms.
 * Duration is rounded up to the configured synthetic input read period.
 * @param x0 Start horizontal coordinate.
 * @param y0 Start vertical coordinate.
 * @param x1 End horizontal coordinate.
 * @param y1 End vertical coordinate.
 * @param duration_ms Minimum gesture duration in milliseconds.
 * @return ESP_OK, ESP_ERR_INVALID_ARG for invalid input, or a queue/lock error.
 */
esp_err_t uitest_swipe(int16_t x0, int16_t y0,
                       int16_t x1, int16_t y1,
                       uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
