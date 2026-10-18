/*
 * SPDX-FileCopyrightText: 2022 Rashed Talukder
 * SPDX-License-Identifier: MIT
 *
 * Internal widget ID registry, accessibility tree snapshot, and state helpers.
 */

#pragma once

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

typedef struct {
    lv_obj_t *obj;
    char id[CONFIG_UITEST_MAX_ID_LENGTH];
} id_entry_t;

static id_entry_t s_registry[CONFIG_UITEST_MAX_REGISTERED];

static void registry_delete_cb(lv_event_t *event)
{
    lv_obj_t *deleted = lv_event_get_target(event);
    for (int i = 0; i < CONFIG_UITEST_MAX_REGISTERED; i++) {
        if (s_registry[i].obj == deleted) {
            s_registry[i].obj = NULL;
            s_registry[i].id[0] = '\0';
        }
    }
}

esp_err_t uitest_register(lv_obj_t *obj, const char *id)
{
    if (obj == NULL || id == NULL || id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t id_length = strnlen(id, CONFIG_UITEST_MAX_ID_LENGTH);
    if (id_length >= CONFIG_UITEST_MAX_ID_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < id_length; i++) {
        if (isspace((unsigned char)id[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!lvgl_port_lock(CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    int available = -1;
    int existing = -1;
    for (int i = 0; i < CONFIG_UITEST_MAX_REGISTERED; i++) {
        if (s_registry[i].obj == obj) {
            existing = i;
        }
        if (s_registry[i].obj != NULL && s_registry[i].obj != obj &&
            strcmp(s_registry[i].id, id) == 0) {
            lvgl_port_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        if (available < 0 && s_registry[i].obj == NULL) {
            available = i;
        }
    }
    if (existing >= 0) {
        memcpy(s_registry[existing].id, id, id_length + 1);
        lvgl_port_unlock();
        return ESP_OK;
    }
    if (available < 0) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    if (lv_obj_add_event_cb(obj, registry_delete_cb, LV_EVENT_DELETE, NULL) == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_registry[available].obj = obj;
    memcpy(s_registry[available].id, id, id_length + 1);
    lvgl_port_unlock();
    return ESP_OK;
}

static lv_obj_t *registry_find_locked(const char *id)
{
    for (int i = 0; i < CONFIG_UITEST_MAX_REGISTERED; i++) {
        if (s_registry[i].obj != NULL && s_registry[i].id[0] != '\0' &&
            strcmp(s_registry[i].id, id) == 0) {
            return s_registry[i].obj;
        }
    }
    return NULL;
}

static void registry_id_copy_locked(lv_obj_t *obj, char *id)
{
    strcpy(id, "-");
    for (int i = 0; i < CONFIG_UITEST_MAX_REGISTERED; i++) {
        if (s_registry[i].obj == obj && s_registry[i].id[0] != '\0') {
            strcpy(id, s_registry[i].id);
            return;
        }
    }
}

typedef struct {
    lv_obj_t *obj;
    uint16_t depth;
} dump_pending_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint16_t depth;
    bool clickable;
    bool hidden;
    char id[CONFIG_UITEST_MAX_ID_LENGTH];
} dump_node_t;

static esp_err_t dump_snapshot(dump_node_t *nodes, size_t *node_count,
                               bool *truncated)
{
    dump_pending_t *pending = calloc(CONFIG_UITEST_MAX_DUMP_NODES,
                                     sizeof(dump_pending_t));
    if (pending == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!lvgl_port_lock(CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS)) {
        free(pending);
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_screen_active();
    size_t count = 0;
    size_t pending_count = 0;
    *truncated = false;
    if (screen != NULL) {
        pending[pending_count++] = (dump_pending_t) { .obj = screen, .depth = 0 };
    }

    while (pending_count > 0 && count < CONFIG_UITEST_MAX_DUMP_NODES) {
        dump_pending_t current = pending[--pending_count];
        lv_area_t area;
        lv_obj_get_coords(current.obj, &area);

        dump_node_t *node = &nodes[count++];
        node->x = area.x1;
        node->y = area.y1;
        node->width = area.x2 - area.x1 + 1;
        node->height = area.y2 - area.y1 + 1;
        node->depth = current.depth;
        node->clickable = lv_obj_is_clickable(current.obj);
        node->hidden = !lv_obj_is_visible(current.obj);
        registry_id_copy_locked(current.obj, node->id);

        uint32_t children = lv_obj_get_child_count(current.obj);
        if (current.depth >= CONFIG_UITEST_MAX_DUMP_DEPTH) {
            if (children > 0) *truncated = true;
            continue;
        }
        for (uint32_t i = children; i > 0; i--) {
            if (pending_count >= CONFIG_UITEST_MAX_DUMP_NODES) {
                *truncated = true;
                break;
            }
            pending[pending_count++] = (dump_pending_t) {
                .obj = lv_obj_get_child(current.obj, i - 1),
                .depth = (uint16_t)(current.depth + 1),
            };
        }
    }
    if (pending_count > 0) *truncated = true;
    lvgl_port_unlock();
    free(pending);
    *node_count = count;
    return ESP_OK;
}

static bool widget_enabled(lv_obj_t *obj)
{
    for (lv_obj_t *ancestor = obj; ancestor != NULL; ancestor = lv_obj_get_parent(ancestor)) {
        if (lv_obj_has_state(ancestor, LV_STATE_DISABLED)) return false;
    }
    return true;
}
