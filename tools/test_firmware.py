# SPDX-FileCopyrightText: 2026 Rashed Talukder
# SPDX-License-Identifier: Apache-2.0

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which("cc"), "native C compiler required")
class FirmwareRegistryTests(unittest.TestCase):
    def test_cancel_waits_for_pointer_reset(self):
        source = (Path(__file__).resolve().parents[1] / "uitest.c").read_text()
        implementation = source[source.index("esp_err_t uitest_cancel(void)\n{"):source.index("esp_err_t uitest_tap(")]
        program = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define pdTRUE 1
#define pdMS_TO_TICKS(value) (value)
#define CONFIG_UITEST_DRAIN_TIMEOUT_MS 100
#define CONFIG_UITEST_SETTLE_MS 10
#define READ_PERIOD 30
static atomic_bool s_ready, s_release_pending;
static void *s_sample_q = (void *)1;
static void *s_inject_lock = (void *)1;
static unsigned int waits, queued;
static bool process_reset;
static int xSemaphoreTake(void *lock, int timeout) {return pdTRUE;}
static void xSemaphoreGive(void *lock) {}
static void xQueueReset(void *queue) {queued = 0;}
static unsigned int uxQueueMessagesWaiting(void *queue) {return queued;}
static unsigned int ulTaskNotifyTake(int clear, int timeout) {
    if (timeout == 0) return 0;
    waits++;
    if (process_reset) {atomic_store(&s_release_pending, false); return 1;}
    return 0;
}
static void vTaskDelay(int ticks) {}
""" + implementation + r"""
int main(void) {
    assert(uitest_cancel() == ESP_ERR_INVALID_STATE);
    atomic_store(&s_ready, true); queued = 10;
    assert(uitest_cancel() == ESP_OK && queued == 0 && atomic_load(&s_release_pending));
    process_reset = true;
    assert(wait_drained() == ESP_OK && waits == 1 && !atomic_load(&s_release_pending));
    process_reset = false;
    assert(uitest_cancel() == ESP_OK);
    assert(wait_drained() == ESP_ERR_TIMEOUT && atomic_load(&s_release_pending));
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "cancel")
            subprocess.run(["cc", "-x", "c", "-std=c11", "-fsanitize=address,undefined", "-o", executable, "-"],
                           input=program.encode(), check=True, capture_output=True)
            subprocess.run([executable], check=True, capture_output=True)

    def test_registry_rename_and_argument_boundaries(self):
        source = (Path(__file__).resolve().parents[1] / "uitest.c").read_text()
        registry = source[source.index("esp_err_t uitest_register("):source.index("static lv_obj_t *registry_find_locked")]
        parser = source[source.index("static const char *skip_word"):source.index("static bool parse_id_arg")]
        program = r"""
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
typedef int esp_err_t;
typedef int lv_obj_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_TIMEOUT 5
#define CONFIG_UITEST_MAX_REGISTERED 4
#define CONFIG_UITEST_MAX_ID_LENGTH 16
#define CONFIG_UITEST_LVGL_LOCK_TIMEOUT_MS 100
#define LV_EVENT_DELETE 1
static struct {lv_obj_t *obj; char id[16];} s_registry[4];
static bool lvgl_port_lock(int timeout) {return true;}
static void lvgl_port_unlock(void) {}
static void registry_delete_cb(void) {}
static void *lv_obj_add_event_cb(lv_obj_t *obj, void (*callback)(void), int event, void *data) {return obj;}
""" + registry + parser + r"""
int main(void) {
    lv_obj_t first = 1, second = 2;
    assert(uitest_register(&first, "first") == ESP_OK);
    assert(uitest_register(&second, "second") == ESP_OK);
    assert(uitest_register(&first, "second") == ESP_ERR_INVALID_STATE);
    assert(strcmp(s_registry[0].id, "first") == 0);
    assert(uitest_register(&first, "renamed") == ESP_OK);
    assert(uitest_register(&first, "renamed") == ESP_OK);
    assert(uitest_register(&second, "renamed") == ESP_ERR_INVALID_STATE);
    assert(uitest_register(&first, "has space") == ESP_ERR_INVALID_ARG);
    int32_t values[2];
    assert(parse_int_args("TAP 10 20", values, 2));
    assert(values[0] == 10 && values[1] == 20);
    assert(!parse_int_args("TAP 10+20", values, 2));
    assert(!parse_int_args("TAP 2147483648 0", values, 2));
    assert(!parse_int_args("TAP 0 0 extra", values, 2));
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "registry")
            subprocess.run(["cc", "-x", "c", "-std=c11", "-fsanitize=address,undefined", "-o", executable, "-"],
                           input=program.encode(), check=True, capture_output=True)
            subprocess.run([executable], check=True, capture_output=True)


if __name__ == "__main__":
    unittest.main()