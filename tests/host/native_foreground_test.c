#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_native_app_abi.h"
#define FOREGROUND_STOP_WAIT_US 5000000ULL
#define SOLAR_OS_TASK_STOP_POLL_MS 20U
#define SOLAR_OS_TASK_REAP_WAIT_MS 100U
#define SOLAR_OS_PACKAGE_SERVICE_AUDIO 1
#define pdMS_TO_TICKS(ms) ((ms) / 10U)
typedef void *TaskHandle_t;
typedef unsigned TickType_t;
typedef struct { void **state_slot; } solar_os_app_t;
typedef struct { solar_os_app_t *app; void *gfx; bool streaming; } solar_os_context_t;
typedef struct { int unused; } solar_os_app_registry_entry_t;
typedef struct { solar_os_native_app_manifest_v2_t manifest; bool legacy; atomic_bool active;
    solar_os_app_registry_entry_t registry; char owner[32]; } foreground_slot_t;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int foreground_slots_lock;
static size_t strlcpy(char *out, const char *in, size_t size) {
    size_t n=strlen(in); if(size) {size_t k=n<size-1?n:size-1; memcpy(out,in,k);out[k]=0;} return n;
}
typedef struct {
    const foreground_slot_t *slot;
    _Atomic(TaskHandle_t) worker;
    atomic_bool done, resumable, stop_requested, suspend_requested, suspended, input_reset;
    uint64_t stop_deadline_us;
    bool worker_started, keyboard_captured;
    void *gfx, *keys, *presenter, *audio;
} foreground_state_t;
static foreground_state_t first, second;
static foreground_slot_t slots[2];
static void *states[2];
static solar_os_app_t apps[2];
static solar_os_context_t contexts[2];
static foreground_state_t *scope, *wait_state;
static TaskHandle_t current;
static int token[3], mode;
static uint64_t clock_us;
static unsigned sequence, audio_order, presenter_order, release_order, deletions;
static bool presenter_fail;
static jmp_buf reboot;
static const solar_os_native_foreground_api_v1_t foreground_api = {0};
static void *solar_os_native_run_service_user(void) { return scope; }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
static uint64_t foreground_time_us(void) { return clock_us; }
static void foreground_audio_close(void) { scope->audio = NULL; audio_order = ++sequence; }
static void solar_os_audio_stream_close(void *audio) { assert(audio); audio_order = ++sequence; }
static int solar_os_frame_presenter_deinit(void *presenter) {
    assert(presenter); presenter_order = ++sequence; return presenter_fail ? ESP_FAIL : ESP_OK;
}
static int solar_os_frame_presenter_suspend_checked(void *presenter) {
    assert(presenter); presenter_order = ++sequence; return presenter_fail ? ESP_FAIL : ESP_OK;
}
static int solar_os_frame_presenter_resume(void *presenter) {
    assert(presenter && contexts[0].streaming); return ESP_OK;
}
static void esp_restart(void) { longjmp(reboot, 1); }
static void vTaskDelay(TickType_t ticks) {
    assert(ticks); clock_us += ticks * 10000U;
    if (mode == 1) atomic_store(&wait_state->suspended, true);
    if (mode == 2) atomic_store(&wait_state->suspended, false);
    if (mode == 3) atomic_store(&wait_state->done, true);
    if (mode == 4) atomic_store(&wait_state->suspend_requested, false);
    if (mode == 5) atomic_store(&wait_state->stop_requested, true);
}
static void foreground_sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void solar_os_input_release_keyboard(void *owner) {
    foreground_state_t *s = owner;
    assert(atomic_load(&s->done) || atomic_load(&s->suspended)); release_order = ++sequence;
}
static void solar_os_context_set_streaming_graphics_active(solar_os_context_t *ctx, bool active) {
    ctx->streaming = active;
}
static void xQueueReset(void *queue) { assert(queue); }
static void vQueueDelete(void *queue) { assert(queue); deletions++; }
static foreground_slot_t *foreground_find_slot(const solar_os_app_t *app) {
    return app == &apps[0] ? &slots[0] : app == &apps[1] ? &slots[1] : NULL;
}
#include "foreground.inc"
static void setup(void) {
    memset(&first, 0, sizeof(first)); memset(&second, 0, sizeof(second));
    memset(slots, 0, sizeof(slots));
    foreground_state_t *s[2] = {&first, &second};
    for (unsigned i = 0; i < 2; ++i) {
        states[i] = s[i]; apps[i].state_slot = &states[i];
        contexts[i] = (solar_os_context_t){.app=&apps[i], .gfx=&token[i], .streaming=true};
        s[i]->slot=&slots[i]; s[i]->keys=&token[i]; s[i]->presenter=&token[i]; s[i]->audio=&token[i];
        slots[i].manifest.flags=SOLAR_OS_NATIVE_APP_RESUMABLE;
        atomic_store(&slots[i].active, true); atomic_store(&s[i]->resumable, true);
        atomic_store(&s[i]->worker, &token[i]); s[i]->worker_started=true;
    }
    scope=&first; current=&token[0]; wait_state=&first;
    clock_us=0; mode=0; presenter_fail=false;
    sequence=audio_order=presenter_order=release_order=deletions=0;
}
int main(void) {
    setup(); char busy[32];
    assert(solar_os_native_foreground_entry(&apps[0]) == &slots[0].registry);
    assert(solar_os_native_foreground_claim(&apps[0], "session1", busy, sizeof(busy)) == ESP_OK);
    assert(solar_os_native_foreground_claim(&apps[1], "session2", busy, sizeof(busy)) == ESP_OK);
    assert(solar_os_native_foreground_claim(&apps[0], "other", busy, sizeof(busy)) == ESP_ERR_INVALID_STATE);
    assert(!strcmp(busy, "session1"));
    solar_os_native_foreground_release(&apps[0], "other");
    assert(solar_os_native_foreground_owner(&apps[0], busy, sizeof(busy)));
    solar_os_native_foreground_release(&apps[0], "session1");
    assert(!solar_os_native_foreground_owner(&apps[0], busy, sizeof(busy)));
    assert(solar_os_native_foreground_owner(&apps[1], busy, sizeof(busy)));
    assert(foreground_context_state(&contexts[0])==&first);
    assert(foreground_context_state(&contexts[1])==&second);
    assert(foreground_caller());
    assert(foreground_get_service("foreground", 1, sizeof(foreground_api), &first)==&foreground_api);
    assert(!foreground_get_service("foreground", 1, sizeof(foreground_api), &second));
    current=&token[1]; assert(!foreground_caller()); assert(!foreground_suspend_requested());
    assert(foreground_enable_suspend()==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    scope=&second; assert(foreground_caller());
    assert(foreground_get_service("foreground", 1, sizeof(foreground_api), &second)==&foreground_api);
    assert(!foreground_get_service("foreground", 2, sizeof(foreground_api), &second));
    assert(!foreground_get_service("foreground", 1, sizeof(foreground_api)+1, &second));
    assert(!foreground_get_service("bogus", 1, 0, &second));
    slots[1].manifest.flags=0; assert(foreground_enable_suspend()==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    setup(); current=&token[2]; mode=1;
    foreground_suspend(&contexts[0]);
    assert(atomic_load(&first.suspended) && !contexts[0].streaming);
    assert(!atomic_load(&first.done) && atomic_load(&first.worker)==&token[0]);
    assert(!atomic_load(&second.suspend_requested) && contexts[1].streaming);
    assert(!deletions && atomic_load(&slots[0].active));
    mode=2; foreground_resume(&contexts[0]);
    assert(contexts[0].streaming && !atomic_load(&first.suspended));
    assert(atomic_load(&first.input_reset) && !atomic_load(&second.input_reset));
    mode=3; foreground_stop(&contexts[0]);
    assert(!atomic_load(&slots[0].active) && atomic_load(&slots[1].active));
    assert(!contexts[0].streaming && contexts[1].streaming && deletions==1);
    assert(!first.keys && second.keys);
    setup(); atomic_store(&first.suspend_requested, true); mode=4;
    assert(foreground_guest_suspend()==SOLAR_OS_NATIVE_OK);
    assert(audio_order < presenter_order && !atomic_load(&first.suspended));
    setup(); atomic_store(&first.suspend_requested, true); mode=5;
    assert(foreground_guest_suspend()==SOLAR_OS_NATIVE_OK);
    assert(atomic_load(&first.stop_requested) && !atomic_load(&first.suspended));
    setup(); foreground_worker_cleanup(&first);
    assert(audio_order < presenter_order && !first.audio && !first.presenter);
    assert(second.audio && second.presenter);
    setup(); presenter_fail=true;
    if (!setjmp(reboot)) { foreground_worker_cleanup(&first); assert(0); }
    setup(); mode=0;
    if (!setjmp(reboot)) { foreground_stop(&contexts[0]); assert(0); }
    assert(clock_us >= FOREGROUND_STOP_WAIT_US && !deletions);
    puts("native foreground lifecycle: OK");
    return 0;
}
