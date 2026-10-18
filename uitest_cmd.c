/*
 * SPDX-FileCopyrightText: 2022 Rashed Talukder
 * SPDX-License-Identifier: MIT
 *
 * UART command listener, @id request envelopes, argument parsing, and SHOT.
 */

#include "uitest_priv.h"

#ifdef CONFIG_UITEST_ENABLED

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"

#ifdef CONFIG_SCREENSHOT_ENABLED
#include "screenshot.h"
#endif

static const char *TAG = "UITEST";

void reply_ok_v(const char *fmt, va_list ap)
{
    flockfile(stdout);
    printf("%s: OK ", RESP_PREFIX);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    funlockfile(stdout);
}

void reply_ok(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    reply_ok_v(fmt, ap);
    va_end(ap);
}

void reply_err(const char *msg)
{
    flockfile(stdout);
    printf("%s: ERR %s\n", RESP_PREFIX, msg);
    fflush(stdout);
    funlockfile(stdout);
}

void reply_command_error(const char *command, esp_err_t err)
{
    flockfile(stdout);
    printf("%s: ERR %s %s\n", RESP_PREFIX, command, esp_err_to_name(err));
    fflush(stdout);
    funlockfile(stdout);
}

static void cmd_info(void)
{
    if (!lvgl_port_lock(CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS)) {
        reply_command_error("INFO", ESP_ERR_TIMEOUT);
        return;
    }
    lv_display_t *display = lv_display_get_default();
    if (display == NULL) {
        lvgl_port_unlock();
        reply_command_error("INFO", ESP_ERR_INVALID_STATE);
        return;
    }
    int32_t w = lv_display_get_horizontal_resolution(display);
    int32_t h = lv_display_get_vertical_resolution(display);
    lv_display_rotation_t rot = lv_display_get_rotation(display);
    lvgl_port_unlock();
    reply_ok("INFO W:%d H:%d ROT:%d PERIOD:%d", (int)w, (int)h,
             (int)rot, READ_PERIOD);
}

static const char *skip_word(const char *cursor)
{
    while (isspace((unsigned char)*cursor)) cursor++;
    while (*cursor != '\0' && !isspace((unsigned char)*cursor)) cursor++;
    while (isspace((unsigned char)*cursor)) cursor++;
    return cursor;
}

static bool command_has_no_args(const char *line)
{
    return *skip_word(line) == '\0';
}

static bool parse_int_args(const char *line, int32_t *values, size_t count)
{
    const char *cursor = skip_word(line);
    for (size_t i = 0; i < count; i++) {
        if (*cursor == '\0') return false;
        errno = 0;
        char *end;
        long value = strtol(cursor, &end, 10);
        if (end == cursor || errno == ERANGE || value < INT32_MIN || value > INT32_MAX ||
            (*end != '\0' && !isspace((unsigned char)*end))) {
            return false;
        }
        values[i] = (int32_t)value;
        cursor = end;
        while (isspace((unsigned char)*cursor)) cursor++;
    }
    return *cursor == '\0';
}

static bool parse_id_arg(const char *line, char *id, size_t id_size)
{
    const char *cursor = skip_word(line);
    const char *end = cursor;
    while (*end != '\0' && !isspace((unsigned char)*end)) end++;
    size_t length = (size_t)(end - cursor);
    while (isspace((unsigned char)*end)) end++;
    if (length == 0 || length >= id_size || *end != '\0') return false;
    memcpy(id, cursor, length);
    id[length] = '\0';
    return true;
}

static bool parse_verb(const char *line, char *verb, size_t verb_size)
{
    while (isspace((unsigned char)*line)) line++;
    const char *end = line;
    while (*end != '\0' && !isspace((unsigned char)*end)) end++;
    size_t length = (size_t)(end - line);
    if (length == 0 || length >= verb_size) return false;
    memcpy(verb, line, length);
    verb[length] = '\0';
    return true;
}

static esp_err_t point_from_int32(int32_t x, int32_t y,
                                  int16_t *point_x, int16_t *point_y)
{
    if (x < INT16_MIN || x > INT16_MAX || y < INT16_MIN || y > INT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *point_x = (int16_t)x;
    *point_y = (int16_t)y;
    return ESP_OK;
}

static void finish_gesture_command(const char *command, esp_err_t err,
                                   const char *reply_format, ...)
{
    if (err == ESP_OK) err = wait_drained();
    if (err != ESP_OK) {
        reply_command_error(command, err);
        return;
    }

    va_list args;
    va_start(args, reply_format);
    reply_ok_v(reply_format, args);
    va_end(args);
}

#ifdef CONFIG_SCREENSHOT_ENABLED
typedef enum {
    SHOT_FULL,
    SHOT_KEY,
    SHOT_DELTA,
    SHOT_AREA,
} shot_kind_t;

#ifdef CONFIG_SCREENSHOT_ASYNC
static SemaphoreHandle_t s_shot_done;
static portMUX_TYPE s_shot_lock = portMUX_INITIALIZER_UNLOCKED;
/* Nonzero while a SHOT waits; the completion that clears it owns the give. */
static uintptr_t s_shot_token;
static uintptr_t s_shot_counter;
static esp_err_t s_shot_result;

static void shot_done(uint32_t sequence, esp_err_t result, void *user_data)
{
    (void)sequence;
    taskENTER_CRITICAL(&s_shot_lock);
    bool mine = s_shot_token != 0 && (uintptr_t)user_data == s_shot_token;
    if (mine) {
        s_shot_token = 0;
        s_shot_result = result;
    }
    taskEXIT_CRITICAL(&s_shot_lock);
    if (mine) xSemaphoreGive(s_shot_done);
}

static esp_err_t shot_wait(shot_kind_t kind, const int32_t *area)
{
    if (s_shot_done == NULL && (s_shot_done = xSemaphoreCreateBinary()) == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (kind == SHOT_KEY) screenshot_delta_reset();

    taskENTER_CRITICAL(&s_shot_lock);
    if (++s_shot_counter == 0) s_shot_counter = 1;
    uintptr_t token = s_shot_token = s_shot_counter;
    taskEXIT_CRITICAL(&s_shot_lock);

    void *context = (void *)token;
    esp_err_t result;
    if (kind == SHOT_AREA) {
        result = screenshot_take_area_async(area[0], area[1], area[2], area[3],
                                            shot_done, context, NULL);
    } else if (kind == SHOT_FULL) {
        result = screenshot_take_async(shot_done, context, NULL);
    } else {
        result = screenshot_take_delta_async(shot_done, context, NULL);
    }
    if (result == ESP_OK &&
        xSemaphoreTake(s_shot_done, pdMS_TO_TICKS(CONFIG_UITEST_SHOT_TIMEOUT_MS)) == pdTRUE) {
        return s_shot_result;
    }

    taskENTER_CRITICAL(&s_shot_lock);
    bool claimed = s_shot_token != token;
    s_shot_token = 0;
    taskEXIT_CRITICAL(&s_shot_lock);
    if (result != ESP_OK) return result;
    if (claimed) {
        /* Completion raced the timeout; its give is imminent. */
        xSemaphoreTake(s_shot_done, portMAX_DELAY);
        return s_shot_result;
    }
    screenshot_cancel_pending();
    return ESP_ERR_TIMEOUT;
}
#else
static esp_err_t shot_wait(shot_kind_t kind, const int32_t *area)
{
    if (kind == SHOT_AREA) return screenshot_take_area(area[0], area[1], area[2], area[3]);
    if (kind == SHOT_FULL) return screenshot_take();
    if (kind == SHOT_KEY) screenshot_delta_reset();
    return screenshot_take_delta();
}
#endif

static void cmd_shot(const char *line)
{
    const char *rest = skip_word(line);
    char mode[8] = "";
    int32_t area[4] = {0};
    shot_kind_t kind = SHOT_FULL;
    if (*rest != '\0' && !parse_verb(rest, mode, sizeof(mode))) {
        mode[0] = '?';
    }
    if (mode[0] == '\0') {
        kind = SHOT_FULL;
    } else if (strcmp(mode, "KEY") == 0 && command_has_no_args(rest)) {
        kind = SHOT_KEY;
    } else if (strcmp(mode, "DELTA") == 0 && command_has_no_args(rest)) {
        kind = SHOT_DELTA;
    } else if (strcmp(mode, "AREA") == 0 && parse_int_args(rest, area, 4)) {
        kind = SHOT_AREA;
    } else {
        reply_err("SHOT [KEY|DELTA|AREA <x> <y> <w> <h>]");
        return;
    }

    esp_err_t err = wait_drained();
    if (err == ESP_OK) err = shot_wait(kind, area);
    if (err != ESP_OK) {
        reply_command_error("SHOT", err);
    } else if (kind == SHOT_AREA) {
        reply_ok("SHOT AREA %ld %ld %ld %ld", (long)area[0], (long)area[1],
                 (long)area[2], (long)area[3]);
    } else {
        reply_ok("SHOT%s%s", mode[0] ? " " : "", mode);
    }
}
#endif

/* Parse and dispatch one command line. */
static void handle_line(char *line)
{
    char verb[CONFIG_UITEST_COMMAND_LINE_LENGTH];
    if (!parse_verb(line, verb, sizeof(verb))) {
        return; /* blank line */
    }

    if (strcmp(verb, "INFO") == 0 && command_has_no_args(line)) {
        cmd_info();

    } else if (strcmp(verb, "DUMP") == 0 && command_has_no_args(line)) {
        esp_err_t err = wait_drained();
        if (err != ESP_OK) {
            reply_command_error("DUMP", err);
            return;
        }
        cmd_dump();

    } else if (strcmp(verb, "TAP") == 0) {
        int32_t args[2];
        int16_t x;
        int16_t y;
        if (!parse_int_args(line, args, 2) ||
            point_from_int32(args[0], args[1], &x, &y) != ESP_OK) {
            reply_err("TAP <x> <y>");
            return;
        }
        finish_gesture_command("TAP", uitest_tap(x, y),
                               "TAP %ld %ld", (long)args[0], (long)args[1]);

    } else if (strcmp(verb, "LONGPRESS") == 0) {
        int32_t args[3];
        int16_t x;
        int16_t y;
        if (!parse_int_args(line, args, 3) || args[2] <= 0 ||
            point_from_int32(args[0], args[1], &x, &y) != ESP_OK) {
            reply_err("LONGPRESS <x> <y> <ms>");
            return;
        }
        finish_gesture_command("LONGPRESS",
                               uitest_long_press(x, y, (uint32_t)args[2]),
                               "LONGPRESS %ld %ld %ld",
                               (long)args[0], (long)args[1], (long)args[2]);

    } else if (strcmp(verb, "SWIPE") == 0) {
        int32_t args[5];
        int16_t x0;
        int16_t y0;
        int16_t x1;
        int16_t y1;
        if (!parse_int_args(line, args, 5) || args[4] <= 0 ||
            point_from_int32(args[0], args[1], &x0, &y0) != ESP_OK ||
            point_from_int32(args[2], args[3], &x1, &y1) != ESP_OK) {
            reply_err("SWIPE <x0> <y0> <x1> <y1> <ms>");
            return;
        }
        finish_gesture_command("SWIPE",
                               uitest_swipe(x0, y0, x1, y1, (uint32_t)args[4]),
                               "SWIPE %ld %ld %ld %ld %ld",
                               (long)args[0], (long)args[1],
                               (long)args[2], (long)args[3], (long)args[4]);

    } else if (strcmp(verb, "GET") == 0) {
        char id[CONFIG_UITEST_MAX_ID_LENGTH];
        if (parse_id_arg(line, id, sizeof(id))) cmd_get(id);
        else reply_err("GET <id>");
    } else if (strcmp(verb, "CLICK") == 0) {
        char id[CONFIG_UITEST_MAX_ID_LENGTH];
        if (parse_id_arg(line, id, sizeof(id))) {
            esp_err_t err = click_id(id);
            finish_gesture_command("CLICK", err, "CLICK %s", id);
        } else {
            reply_err("CLICK <id>");
        }

    } else if (strcmp(verb, "CANCEL") == 0 && command_has_no_args(line)) {
        esp_err_t err = uitest_cancel();
        if (err == ESP_OK) err = wait_drained();
        if (err == ESP_OK) reply_ok("CANCEL");
        else reply_command_error("CANCEL", err);
    } else if (strcmp(verb, "SHOT") == 0) {
#ifdef CONFIG_SCREENSHOT_ENABLED
        cmd_shot(line);
#else
        reply_err("SHOT screenshot component disabled");
#endif

    } else {
        flockfile(stdout);
        printf("%s: ERR %s unknown command or invalid arguments\n",
               RESP_PREFIX, verb);
        fflush(stdout);
        funlockfile(stdout);
    }
}

/* Block until stdin is readable instead of polling; the timeout bounds stop latency. */
static void wait_for_input(void)
{
    int fd = fileno(stdin);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(fd, &readable);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    if (select(fd + 1, &readable, NULL, NULL, &timeout) < 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void listener_task(void *arg)
{
    (void)arg;
    char line[CONFIG_UITEST_COMMAND_LINE_LENGTH];
    size_t length = 0;
    bool overflow = false;

    atomic_store(&s_listener_task, xTaskGetCurrentTaskHandle());
    ESP_LOGI(TAG, "UI test harness ready — commands: "
                  "INFO DUMP TAP LONGPRESS SWIPE CLICK GET CANCEL SHOT");

    while (!atomic_load(&s_stopping)) {
        int character = fgetc(stdin);
        if (character == EOF) {
            clearerr(stdin);
            wait_for_input();
            continue;
        }
        if (character == '\r') continue;
        if (character == '\n') {
            if (overflow) {
                reply_err("command too long");
            } else if (length > 0) {
                line[length] = '\0';
                if (line[0] == '@') {
                    char *end;
                    errno = 0;
                    unsigned long request = strtoul(line + 1, &end, 10);
                    if (errno != 0 || request == 0 || !isdigit((unsigned char)line[1]) || *end != ' ') {
                        reply_err("invalid request id");
                    } else {
                        flockfile(stdout);
                        printf("%s: BEGIN %lu\n", RESP_PREFIX, request);
                        fflush(stdout);
                        funlockfile(stdout);
                        handle_line(end + 1);
                        flockfile(stdout);
                        printf("%s: END %lu\n", RESP_PREFIX, request);
                        fflush(stdout);
                        funlockfile(stdout);
                    }
                } else {
                    handle_line(line);
                }
            }
            length = 0;
            overflow = false;
            continue;
        }
        if (length + 1 < sizeof(line)) {
            line[length++] = (char)character;
        } else {
            overflow = true;
        }
    }

    atomic_store(&s_listener_task, NULL);
    atomic_store(&s_listener_running, false);
    vTaskDelete(NULL);
}

#endif /* CONFIG_UITEST_ENABLED */
