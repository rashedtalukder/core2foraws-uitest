/*
 * SPDX-FileCopyrightText: 2022 Rashed Talukder
 * SPDX-License-Identifier: MIT
 *
 * Core2 for AWS UI Test Harness
 */

#include "uitest_priv.h"

#ifdef CONFIG_UITEST_ENABLED

#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "UITEST";

static atomic_flag s_init_busy = ATOMIC_FLAG_INIT;
atomic_bool s_stopping;
atomic_bool s_listener_running;

/* ── Initialization ─────────────────────────────────────────────── */

static esp_err_t init_impl(void)
{
    if (RESP_PREFIX[0] == '\0' || strpbrk(RESP_PREFIX, ":\r\n\t ") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (atomic_load(&s_listener_running)) return ESP_OK;
    if (atomic_load(&s_stopping)) return ESP_ERR_INVALID_STATE;

    /* The queue and mutex outlive deinit so late API callers never touch freed handles. */
    if (s_sample_q == NULL) {
        s_sample_q = xQueueCreate(CONFIG_UITEST_SAMPLE_QUEUE_DEPTH,
                                  sizeof(inject_sample_t));
        if (s_sample_q == NULL) {
            ESP_LOGE(TAG, "Failed to allocate sample queue");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_inject_lock == NULL) {
        s_inject_lock = xSemaphoreCreateMutex();
        if (s_inject_lock == NULL) {
            ESP_LOGE(TAG, "Failed to allocate injection mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_indev == NULL) {
        if (!lvgl_port_lock(CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "Timed out acquiring LVGL lock during initialization");
            return ESP_ERR_TIMEOUT;
        }
        lv_display_t *display = lv_display_get_default();
        if (display == NULL) {
            lvgl_port_unlock();
            return ESP_ERR_INVALID_STATE;
        }

        s_last_x = (int16_t)(lv_display_get_horizontal_resolution(display) / 2);
        s_last_y = (int16_t)(lv_display_get_vertical_resolution(display) / 2);
        s_indev = lv_indev_create();
        if (s_indev == NULL) {
            lvgl_port_unlock();
            ESP_LOGE(TAG, "Failed to create synthetic indev");
            return ESP_ERR_NO_MEM;
        }
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, indev_read_cb);
        lv_indev_set_display(s_indev, display);
        lv_timer_t *read_timer = lv_indev_get_read_timer(s_indev);
        if (read_timer == NULL) {
            lv_indev_delete(s_indev);
            s_indev = NULL;
            lvgl_port_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        lv_timer_set_period(read_timer, READ_PERIOD);
        lvgl_port_unlock();
    }

    atomic_store(&s_ready, true);
    atomic_store(&s_listener_running, true);
    TaskHandle_t listener = NULL;
    BaseType_t ok = xTaskCreatePinnedToCore(
        listener_task, "uitest", CONFIG_UITEST_TASK_STACK_SIZE,
        NULL, 2, &listener, 0);
    if (ok != pdPASS) {
        atomic_store(&s_listener_running, false);
        atomic_store(&s_ready, false);
        ESP_LOGE(TAG, "Failed to start listener task");
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_listener_task, listener);

    ESP_LOGI(TAG, "UI test harness initialized");
    return ESP_OK;
}

esp_err_t uitest_init(void)
{
    if (atomic_flag_test_and_set(&s_init_busy)) return ESP_ERR_INVALID_STATE;
    esp_err_t result = init_impl();
    atomic_flag_clear(&s_init_busy);
    return result;
}

esp_err_t uitest_deinit(void)
{
    TaskHandle_t listener = atomic_load(&s_listener_task);
    if (listener != NULL && xTaskGetCurrentTaskHandle() == listener) {
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_flag_test_and_set(&s_init_busy)) return ESP_ERR_INVALID_STATE;
    esp_err_t result = ESP_OK;
    atomic_store(&s_ready, false);
    atomic_store(&s_stopping, true);

    /* A listener blocked in a gesture drain exits once that drain times out. */
    uint32_t limit = CONFIG_UITEST_DRAIN_TIMEOUT_MS + CONFIG_UITEST_SETTLE_MS + 500;
    for (uint32_t waited = 0; atomic_load(&s_listener_running); waited += 10) {
        if (waited >= limit) {
            result = ESP_ERR_TIMEOUT;
            goto done;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_indev != NULL) {
        if (!lvgl_port_lock(CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS)) {
            result = ESP_ERR_TIMEOUT;
            goto done;
        }
        lv_indev_delete(s_indev);
        s_indev = NULL;
        lvgl_port_unlock();
    }
    if (s_inject_lock != NULL &&
        xSemaphoreTake(s_inject_lock, pdMS_TO_TICKS(CONFIG_UITEST_DRAIN_TIMEOUT_MS)) == pdTRUE) {
        xQueueReset(s_sample_q);
        xSemaphoreGive(s_inject_lock);
    }
    atomic_store(&s_release_pending, false);
    atomic_store(&s_stopping, false);
done:
    atomic_flag_clear(&s_init_busy);
    return result;
}

#else

esp_err_t uitest_deinit(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t uitest_cancel(void) { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t uitest_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uitest_register(lv_obj_t *obj, const char *id)
{
    (void)obj;
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uitest_tap(int16_t x, int16_t y)
{
    (void)x;
    (void)y;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uitest_long_press(int16_t x, int16_t y, uint32_t hold_ms)
{
    (void)x;
    (void)y;
    (void)hold_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uitest_swipe(int16_t x0, int16_t y0,
                       int16_t x1, int16_t y1,
                       uint32_t duration_ms)
{
    (void)x0;
    (void)y0;
    (void)x1;
    (void)y1;
    (void)duration_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_UITEST_ENABLED */
