#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_native_app_abi.h"
#define SOLAR_OS_PACKAGE_SERVICE_SYNTH 1
#define SOLAR_OS_PACKAGE_SERVICE_AUDIO 1
#define portMAX_DELAY 0U
#define pdTRUE 1
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef struct { solar_os_native_app_manifest_v2_t manifest; } foreground_slot_t;
typedef struct {
    foreground_slot_t *slot;
    _Atomic(TaskHandle_t) worker;
    atomic_bool done;
    SemaphoreHandle_t synth_mutex;
    bool synth_running;
    solar_os_native_foreground_audio_render_v1_t synth_render;
    void *synth_user, *audio, *presenter;
    uint16_t palette[256];
} foreground_state_t;
typedef struct {
    const char *owner;
    solar_os_native_foreground_audio_render_v1_t render;
    void *user;
    size_t block_frames;
} solar_os_synth_config_t;
typedef struct { uint32_t sample_rate; char owner[32]; } solar_os_synth_status_t;
static char synth_owner[32];
static foreground_state_t state, other;
static foreground_slot_t slot;
static solar_os_synth_config_t config;
static int token[3], depth, calls, starts, stops, deleted, presented;
static foreground_state_t *scope;
static TaskHandle_t current;
static bool mutex_fail, start_fail, stop_fail, image_live;
static uint32_t sample_rate=16000;
static jmp_buf restarted;
static void *solar_os_native_run_service_user(void) { return scope; }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current; }
static int solar_os_display_get_colors(uint32_t *fg, uint32_t *bg) {*fg=0x112233;*bg=0xddeeff;return ESP_OK;}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {return mutex_fail?NULL:&token[2];}
static int xSemaphoreTake(SemaphoreHandle_t mutex,unsigned timeout) {
    (void)timeout;assert(mutex && !depth);depth++;return pdTRUE;
}
static int xSemaphoreGive(SemaphoreHandle_t mutex) {assert(mutex && depth==1);depth--;return pdTRUE;}
static void vSemaphoreDelete(SemaphoreHandle_t mutex) {assert(mutex && !depth && !state.synth_running);deleted++;}
static int solar_os_synth_start(const solar_os_synth_config_t *c) {starts++;config=*c;return start_fail?ESP_FAIL:ESP_OK;}
static int solar_os_synth_stop(const char *owner) {
    assert(!strcmp(owner,"gameboy"));stops++;
    if(stop_fail)return ESP_ERR_TIMEOUT;
    int16_t samples[32]={0};config.render(samples,16,16000,config.user);return ESP_OK;
}
static void solar_os_synth_get_status(solar_os_synth_status_t *s) {s->sample_rate=sample_rate;strcpy(s->owner,synth_owner);}
static void solar_os_audio_stream_close(void *s) {(void)s;assert(!state.synth_running);}
static int solar_os_frame_presenter_deinit(void *p) {assert(p && !state.synth_running && deleted);presented++;return ESP_OK;}
static void esp_restart(void) {longjmp(restarted,1);}
#include "audio.inc"
static void guest_render(int16_t *samples,size_t frames,uint32_t rate,void *user) {
    assert(image_live && samples && frames==16 && rate==16000 && user==&state && depth==1);calls++;
}
static void setup(void) {
    memset(&state,0,sizeof(state));memset(&other,0,sizeof(other));memset(&slot,0,sizeof(slot));
    strcpy(slot.manifest.id,"gameboy");state.slot=&slot;
    atomic_store(&state.worker,&token[0]);atomic_store(&other.worker,&token[1]);
    strcpy(synth_owner,"gameboy");scope=&state;current=&token[0];mutex_fail=start_fail=stop_fail=false;image_live=true;
    starts=stops=deleted=presented=calls=depth=0;sample_rate=16000;
}
int main(void) {
    setup();foreground_refresh_index2_palette(&state);
    assert(state.palette[0]==foreground_rgb565(0xddeeff));
    assert(state.palette[3]==foreground_rgb565(0x112233));
    assert(state.palette[1]==foreground_rgb565(0x99aabb));
    assert(foreground_synth_open(guest_render,&state,16,16000)==SOLAR_OS_NATIVE_OK);
    assert(config.render!=guest_render && config.user==&state);
    assert(foreground_synth_lock());foreground_synth_unlock();
    current=&token[1];assert(!foreground_synth_lock());foreground_synth_close();assert(!stops);
    assert(foreground_synth_open(guest_render,&state,16,16000)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    current=&token[2];int16_t samples[32]={0};config.render(samples,16,16000,config.user);assert(calls==1);
    current=&token[0];foreground_synth_close();assert(!state.synth_running && stops==1 && calls==2);
    assert(state.synth_mutex && !deleted); /* retained across suspension */
    assert(foreground_synth_open(guest_render,&state,16,16000)==SOLAR_OS_NATIVE_OK);
    state.presenter=&token[2];foreground_worker_cleanup(&state);
    assert(stops==2 && deleted==1 && presented==1 && !state.synth_mutex && !state.synth_render);
    image_live=false;
    setup();assert(foreground_synth_open(guest_render,&state,16,16000)==SOLAR_OS_NATIVE_OK);
    strcpy(synth_owner,"other");image_live=false;foreground_worker_cleanup(&state);
    assert(!stops && deleted==1 && !state.synth_render);
    setup();mutex_fail=true;assert(foreground_synth_open(guest_render,&state,16,16000)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(!starts && !state.synth_render);
    setup();start_fail=true;assert(foreground_synth_open(guest_render,&state,16,16000)==SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!state.synth_running && !state.synth_render);foreground_worker_cleanup(&state);assert(deleted==1);
    setup();sample_rate=48000;assert(foreground_synth_open(guest_render,&state,16,16000)==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(stops==1 && !state.synth_running);
    setup();assert(foreground_synth_open(guest_render,&state,16,16000)==SOLAR_OS_NATIVE_OK);stop_fail=true;
    if(!setjmp(restarted)){foreground_worker_cleanup(&state);assert(0);}
    assert(!deleted && state.synth_render && image_live);
    puts("foreground synth callbacks join before image unload: OK");return 0;
}
