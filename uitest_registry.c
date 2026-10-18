/*
 * SPDX-FileCopyrightText: 2022 Rashed Talukder
 * SPDX-License-Identifier: MIT
 *
 * Widget ID registry, accessibility tree dump, state queries, and CLICK.
 */

#include "uitest_priv.h"

#ifdef CONFIG_UITEST_ENABLED

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"
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

void cmd_dump(void)
{
    dump_node_t *nodes = calloc(CONFIG_UITEST_MAX_DUMP_NODES,
                                sizeof(dump_node_t));
    if (nodes == NULL) {
        flockfile(stdout);
        printf("---%s_DUMP_START---\n", RESP_PREFIX);
        printf("%s: ERR DUMP %s\n", RESP_PREFIX,
               esp_err_to_name(ESP_ERR_NO_MEM));
        printf("---%s_DUMP_END---\n", RESP_PREFIX);
        fflush(stdout);
        funlockfile(stdout);
        return;
    }
    size_t node_count = 0;
    bool truncated = false;
    esp_err_t err = dump_snapshot(nodes, &node_count, &truncated);

    flockfile(stdout);
    printf("---%s_DUMP_START---\n", RESP_PREFIX);
    if (err == ESP_OK) {
        uint32_t crc = 0;
        char line[CONFIG_UITEST_MAX_ID_LENGTH + 128];
        for (size_t i = 0; i < node_count; i++) {
            dump_node_t *node = &nodes[i];
            int length = snprintf(
                line, sizeof(line),
                "%s: NODE d:%u id:%s x:%ld y:%ld w:%ld h:%ld click:%d hidden:%d\n",
                RESP_PREFIX, node->depth, node->id,
                (long)node->x, (long)node->y,
                (long)node->width, (long)node->height,
                node->clickable, node->hidden);
            if (length < 0 || (size_t)length >= sizeof(line)) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            fwrite(line, 1, (size_t)length, stdout);
            crc = esp_rom_crc32_le(crc, (const uint8_t *)line, (uint32_t)length);
        }
        if (err == ESP_OK && truncated) {
            printf("%s: WARN DUMP truncated at %d nodes/depth %d\n",
                   RESP_PREFIX, CONFIG_UITEST_MAX_DUMP_NODES,
                   CONFIG_UITEST_MAX_DUMP_DEPTH);
        }
        if (err == ESP_OK) {
            printf("%s: DUMP CRC32:%08lx COUNT:%lu TRUNCATED:%d\n",
                   RESP_PREFIX, (unsigned long)crc,
                   (unsigned long)node_count, truncated);
        }
    }
    if (err != ESP_OK) {
        printf("%s: ERR DUMP %s\n", RESP_PREFIX, esp_err_to_name(err));
    }
    printf("---%s_DUMP_END---\n", RESP_PREFIX);
    fflush(stdout);
    funlockfile(stdout);
    free(nodes);
}

static bool widget_enabled(lv_obj_t *obj)
{
    for (lv_obj_t *ancestor = obj; ancestor != NULL; ancestor = lv_obj_get_parent(ancestor)) {
        if (lv_obj_has_state(ancestor, LV_STATE_DISABLED)) return false;
    }
    return true;
}

void cmd_get(const char *id)
{
    if (!lvgl_port_lock(CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS)) {
        reply_command_error("GET", ESP_ERR_TIMEOUT);
        return;
    }
    lv_obj_t *obj = registry_find_locked(id);
    if (obj == NULL) {
        lvgl_port_unlock();
        reply_command_error("GET", ESP_ERR_NOT_FOUND);
        return;
    }
    lv_obj_update_layout(obj);
    bool visible = lv_obj_is_visible(obj) && lv_obj_get_screen(obj) == lv_screen_active();
    bool enabled = widget_enabled(obj);
    bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
    int32_t value = 0;
    const char *text = "";
    if (lv_obj_check_type(obj, &lv_label_class)) text = lv_label_get_text(obj);
    else if (lv_obj_check_type(obj, &lv_textarea_class)) text = lv_textarea_get_text(obj);
    else if (lv_obj_check_type(obj, &lv_slider_class)) value = lv_slider_get_value(obj);
    else if (lv_obj_check_type(obj, &lv_roller_class)) value = lv_roller_get_selected(obj);
#if LV_USE_BAR
    else if (lv_obj_check_type(obj, &lv_bar_class)) value = lv_bar_get_value(obj);
#endif
#if LV_USE_ARC
    else if (lv_obj_check_type(obj, &lv_arc_class)) value = lv_arc_get_value(obj);
#endif
#if LV_USE_DROPDOWN
    else if (lv_obj_check_type(obj, &lv_dropdown_class)) value = (int32_t)lv_dropdown_get_selected(obj);
#endif
#if LV_USE_CHECKBOX
    else if (lv_obj_check_type(obj, &lv_checkbox_class)) text = lv_checkbox_get_text(obj);
#endif
    if (text == NULL) text = "";
    size_t length = strnlen(text, 128);
    bool truncated = length >= 128;
    if (truncated) {
        length = 127;
        /* Cut on a UTF-8 boundary so the host can still decode the prefix. */
        while (length > 0 && ((uint8_t)text[length] & 0xC0) == 0x80) length--;
    }
    char encoded[257];
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < length; index++) {
        encoded[index * 2] = hex[(uint8_t)text[index] >> 4];
        encoded[index * 2 + 1] = hex[(uint8_t)text[index] & 15];
    }
    encoded[length * 2] = '\0';
    lvgl_port_unlock();
    reply_ok("GET %s VISIBLE:%d ENABLED:%d CHECKED:%d VALUE:%ld TEXT:%s TRUNCATED:%d",
             id, visible, enabled, checked, (long)value, length ? encoded : "-",
             truncated);
}

/* Center of a registered/clickable object -> a tap. */
esp_err_t click_id(const char *id)
{
    if (!lvgl_port_lock(CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *obj = registry_find_locked(id);
    lv_area_t a = {0};
    if (obj != NULL) lv_obj_update_layout(obj);
    bool actionable = obj != NULL && lv_obj_is_visible(obj) &&
        widget_enabled(obj) &&
        lv_obj_is_clickable(obj) &&
        lv_obj_get_screen(obj) == lv_screen_active();
    if (actionable) {
        lv_obj_get_coords(obj, &a);
        lv_point_t point = {.x = (a.x1 + a.x2) / 2, .y = (a.y1 + a.y2) / 2};
        lv_obj_t *hit = lv_indev_search_obj(lv_layer_sys(), &point);
        if (hit == NULL) hit = lv_indev_search_obj(lv_layer_top(), &point);
        if (hit == NULL) hit = lv_indev_search_obj(lv_screen_active(), &point);
        actionable = hit == obj;
    }
    lvgl_port_unlock();

    if (obj == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!actionable) {
        return ESP_ERR_INVALID_STATE;
    }
    int16_t cx = (int16_t)((a.x1 + a.x2) / 2);
    int16_t cy = (int16_t)((a.y1 + a.y2) / 2);
    return uitest_tap(cx, cy);
}

#endif /* CONFIG_UITEST_ENABLED */
