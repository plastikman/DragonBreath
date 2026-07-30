#include "pb_evlog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static SemaphoreHandle_t s_lock = NULL;
static pb_evlog_entry_t  s_buf[PB_EVLOG_MAX_ENTRIES];
static size_t            s_head = 0;   // next write slot
static size_t            s_count = 0;  // number of valid entries (<= MAX)

// ---- firmware console capture (raw ESP_LOGx byte ring) ----
// Independent of the curated event ring above. A spinlock (not a mutex) guards it
// because the esp_log vprintf hook can be reached from many contexts and must never
// block or assert; the critical sections are a short append (<=CON_LINE bytes) and
// a rarely-called full-ring snapshot. We NEVER log from inside the hook.
#define CON_LINE  200
static portMUX_TYPE     s_con_mux = portMUX_INITIALIZER_UNLOCKED;
static char             s_con[PB_EVLOG_CONSOLE_BYTES];
static size_t           s_con_head = 0;    // next write index
static bool             s_con_full = false;
static vprintf_like_t   s_prev_vprintf = NULL;
static bool             s_con_on = false;

static void con_append(const char *p, int n)
{
    if (n <= 0) return;
    portENTER_CRITICAL(&s_con_mux);
    for (int i = 0; i < n; i++) {
        s_con[s_con_head++] = p[i];
        if (s_con_head >= PB_EVLOG_CONSOLE_BYTES) { s_con_head = 0; s_con_full = true; }
    }
    portEXIT_CRITICAL(&s_con_mux);
}

// esp_log hook: tee the formatted line into the ring, then forward to the original
// writer (UART) via a va_copy so the serial console is unchanged.
static int con_vprintf(const char *fmt, va_list ap)
{
    char line[CON_LINE];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    con_append(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
    int r = s_prev_vprintf ? s_prev_vprintf(fmt, ap2) : 0;
    va_end(ap2);
    return r;
}

void pb_evlog_init(void)
{
    if (s_lock != NULL) return;
    s_lock = xSemaphoreCreateMutex();
    // If mutex creation fails we just stay in the "not initialised" state and
    // pb_evlog_add becomes a no-op — good enough for a diagnostic aid.
}

void pb_evlog_add(const char *fmt, ...)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;

    pb_evlog_entry_t *e = &s_buf[s_head];
    e->ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);

    s_head = (s_head + 1) % PB_EVLOG_MAX_ENTRIES;
    if (s_count < PB_EVLOG_MAX_ENTRIES) s_count++;

    xSemaphoreGive(s_lock);
}

size_t pb_evlog_snapshot(pb_evlog_entry_t *out, size_t max)
{
    if (out == NULL || max == 0) return 0;
    if (s_lock == NULL) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    size_t want = (max < s_count) ? max : s_count;
    // Copy newest-first: walk backward from s_head.
    size_t idx = (s_head == 0) ? (PB_EVLOG_MAX_ENTRIES - 1) : (s_head - 1);
    for (size_t i = 0; i < want; ++i) {
        out[i] = s_buf[idx];
        idx = (idx == 0) ? (PB_EVLOG_MAX_ENTRIES - 1) : (idx - 1);
    }

    xSemaphoreGive(s_lock);
    return want;
}

void pb_evlog_console_init(void)
{
    if (s_con_on) return;
    s_con_on = true;
    s_prev_vprintf = esp_log_set_vprintf(con_vprintf);   // returns the UART writer
}

size_t pb_evlog_console_snapshot(char *out, size_t max)
{
    if (out == NULL || max == 0) return 0;
    size_t cap = max - 1;   // leave room for the NUL
    size_t len = 0;

    portENTER_CRITICAL(&s_con_mux);
    if (s_con_full) {
        // Ring wrapped: oldest byte is at s_con_head. Emit [head..end) then [0..head).
        size_t tail = PB_EVLOG_CONSOLE_BYTES - s_con_head;
        size_t a = tail < cap ? tail : cap;
        memcpy(out, s_con + s_con_head, a);
        len = a;
        size_t b = s_con_head < (cap - len) ? s_con_head : (cap - len);
        memcpy(out + len, s_con, b);
        len += b;
    } else {
        size_t a = s_con_head < cap ? s_con_head : cap;
        memcpy(out, s_con, a);
        len = a;
    }
    portEXIT_CRITICAL(&s_con_mux);

    out[len] = '\0';
    return len;
}
