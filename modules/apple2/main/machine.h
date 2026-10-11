#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_native_app_abi.h"

#define APPLE2_WIDTH 280U
#define APPLE2_HEIGHT 192U
#define APPLE2_NIB_SIZE 232960U
#define APPLE2_CLOCK_HZ 1023000U
#define APPLE2_TEXT_FOREGROUND 32U
#define APPLE2_TEXT_BACKGROUND 33U

typedef struct {
    uint8_t data[APPLE2_NIB_SIZE];
    char filename[160];
    bool mounted, readOnly, motorOn, writeMode, dirty;
    uint8_t track;
    uint16_t nibble;
} apple2_drive_t;

typedef struct {
    uint8_t ram[0xc000], rom[0x3000], lgc[0x3000], bk2[0x1000], sl6[256];
    bool TEXT, MIXED, PAGE2, HIRES, LCWR, LCRD, LCBK2, LCWFF;
    uint8_t KBD, PB0, PB1, PB2, dLatch;
    uint8_t paddle[2];
    uint64_t paddle_deadline[2];
    int curDrv;
    bool phases[2][4], phasesB[2][4], phasesBB[2][4];
    int pIdx[2], pIdxB[2], halfTrackPos[2];
    apple2_drive_t disk[2];
    uint8_t pixels[APPLE2_WIDTH * APPLE2_HEIGHT];
    uint8_t video_graphics[8192], video_text[1024], video_mode;
    bool video_valid, video_flash;
    uint64_t video_status_us;
    uint8_t chars[64], char_read, char_count;
    bool fast_disk, show_stats, show_help, audio_suspended;
    uint32_t disk_reads;
    uint32_t guest_khz, cpu_khz;
    uint8_t video_percent;
    bool paused, muted, speaker, audio_active;
    int32_t speaker_input, speaker_filtered;
    int audio_error;
    uint32_t sample_rate, sample_step, sample_remainder, sample_fraction;
    uint64_t next_sample;
    uint8_t channels;
    size_t pcm_frames;
    int16_t pcm[1024];
} apple2_machine_t;

extern apple2_machine_t *apple2_machine;
extern const solar_os_native_foreground_api_v1_t *apple2_app;
static inline bool apple2_disk_accelerated(void)
{
    return apple2_machine->fast_disk &&
        apple2_machine->disk[apple2_machine->curDrv].mounted &&
        apple2_machine->disk[apple2_machine->curDrv].motorOn;
}
void apple2_machine_init(apple2_machine_t *machine);
uint8_t readMem(uint16_t address);
void writeMem(uint16_t address, uint8_t value);
void apple2_audio_until(uint64_t cycle);
void apple2_video(uint64_t cycle);
bool apple2_video_refresh(uint64_t cycle, uint64_t now_us);
void apple2_palette(uint16_t palette[256]);
