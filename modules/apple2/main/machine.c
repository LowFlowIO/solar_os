/*
 * Reinette II plus, a french Apple II emulator, using SDL2
 * and powered by puce6502 - a MOS 6502 cpu emulator by the same author
 * Last modified 21st of June 2021
 * Copyright (c) 2020 Arthur Ferreira
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */



#include "machine.h"
#include "vendor/puce6502.h"
#include <string.h>

apple2_machine_t *apple2_machine;
const solar_os_native_vm_api_v1_t *apple2_vm;

void apple2_machine_init(apple2_machine_t *machine) {
    memset(machine, 0, sizeof(*machine));
    apple2_machine = machine;
    machine->TEXT = machine->LCWR = machine->LCBK2 = true;
    machine->fast_disk = true;
    machine->paddle[0] = machine->paddle[1] = 127;
    ticks = 0;
}

#define RAMSIZE 0xc000U
#define ROMSTART 0xd000U
#define LGCSTART 0xd000U
#define BK2START 0xd000U
#define SL6START 0xc600U
#define ram (apple2_machine->ram)
#define rom (apple2_machine->rom)
#define lgc (apple2_machine->lgc)
#define bk2 (apple2_machine->bk2)
#define sl6 (apple2_machine->sl6)
#define KBD (apple2_machine->KBD)
#define TEXT (apple2_machine->TEXT)
#define MIXED (apple2_machine->MIXED)
#define PAGE2 (apple2_machine->PAGE2)
#define HIRES (apple2_machine->HIRES)
#define LCWR (apple2_machine->LCWR)
#define LCRD (apple2_machine->LCRD)
#define LCBK2 (apple2_machine->LCBK2)
#define LCWFF (apple2_machine->LCWFF)
#define PB0 (apple2_machine->PB0)
#define PB1 (apple2_machine->PB1)
#define PB2 (apple2_machine->PB2)
#define disk (apple2_machine->disk)
#define curDrv (apple2_machine->curDrv)

static void resetPaddles(void) {
    for (unsigned i = 0; i < 2; ++i)
        apple2_machine->paddle_deadline[i] = ticks +
            (uint32_t)apple2_machine->paddle[i] * apple2_machine->paddle[i] * 66U / 10U;
}
static uint8_t readPaddle(unsigned i) {
    return ticks < apple2_machine->paddle_deadline[i] ? 0x80U : 0U;
}
static void playSound(void) {
    apple2_audio_until(ticks);
    apple2_machine->speaker = !apple2_machine->speaker;
}

void apple2_audio_until(uint64_t cycle) {
    apple2_machine_t *m = apple2_machine;
    if (!m->audio_active || m->audio_error) return;
    if (apple2_disk_accelerated()) {
        if (!m->audio_suspended) {
            memset(m->pcm, 0, sizeof(m->pcm));
            m->audio_error = apple2_vm->audio_write(m->pcm, 512U);
            m->audio_suspended = true;
        }
        m->next_sample = cycle;
        m->pcm_frames = 0;
        return;
    }
    if (m->audio_suspended) {
        m->audio_suspended = false;
        m->next_sample = cycle;
        m->speaker_input = m->speaker ? 6000 : 0;
        m->speaker_filtered = 0;
    }
    while (m->next_sample <= cycle) {
        /* Remove DC: a speaker that stops toggling becomes silent. */
        int32_t input = m->speaker ? 6000 : 0;
        m->speaker_filtered = input - m->speaker_input +
            (m->speaker_filtered * 255) / 256;
        m->speaker_input = input;
        const int16_t value = m->muted ? 0 : (int16_t)m->speaker_filtered;
        for (unsigned channel = 0; channel < m->channels; ++channel)
            m->pcm[m->pcm_frames * m->channels + channel] = value;
        m->pcm_frames++;
        m->next_sample += m->sample_step;
        m->sample_fraction += m->sample_remainder;
        if (m->sample_fraction >= m->sample_rate) {
            m->sample_fraction -= m->sample_rate;
            m->next_sample++;
        }
        if (m->pcm_frames == 512U) {
            m->audio_error = apple2_vm->audio_write(m->pcm, m->pcm_frames);
            m->pcm_frames = 0;
            if (m->audio_error) return;
        }
    }
}
void stepMotor(uint16_t address) {
    bool (*phases)[4] = apple2_machine->phases;                                             // phases states (for both drives)
    bool (*phasesB)[4] = apple2_machine->phasesB;                                            // phases states Before
    bool (*phasesBB)[4] = apple2_machine->phasesBB;                                           // phases states Before Before
    int *pIdx = apple2_machine->pIdx;                                                   // phase index (for both drives)
    int *pIdxB = apple2_machine->pIdxB;                                                  // phase index Before
    int *halfTrackPos = apple2_machine->halfTrackPos;

    address &= 7;
    int phase = address >> 1;

    phasesBB[curDrv][pIdxB[curDrv]] = phasesB[curDrv][pIdxB[curDrv]];
    phasesB[curDrv][pIdx[curDrv]]   = phases[curDrv][pIdx[curDrv]];
    pIdxB[curDrv] = pIdx[curDrv];
    pIdx[curDrv]  = phase;

    if (!(address & 1)) {                                                         // head not moving (PHASE x OFF)
        phases[curDrv][phase] = false;
        return;
    }

    if ((phasesBB[curDrv][(phase + 1) & 3]) && (--halfTrackPos[curDrv] < 0))      // head is moving in
        halfTrackPos[curDrv] = 0;

    if ((phasesBB[curDrv][(phase - 1) & 3]) && (++halfTrackPos[curDrv] > 68))    // head is moving out
        halfTrackPos[curDrv] = 68;

    phases[curDrv][phase] = true;                                                 // update track#
    disk[curDrv].track = (halfTrackPos[curDrv] + 1) / 2;
}


static void setDrv(int drv) {
    disk[drv].motorOn = disk[!drv].motorOn || disk[drv].motorOn;                  // if any of the motors were ON
    disk[!drv].motorOn = false;                                                   // motor of the other drive is set to OFF
    curDrv = drv;                                                                 // set the current drive
}


//========================================== MEMORY MAPPED SOFT SWITCHES HANDLER
// this function is called from readMem and writeMem
// it complements both functions when address is in page $C0
uint8_t softSwitches(uint16_t address, uint8_t value, bool WRT) {
    #define dLatch (apple2_machine->dLatch)                                                    // disk ][ I/O register

    switch (address) {
    case 0xC000: return KBD;                                                    // KEYBOARD
    case 0xC010: KBD &= 0x7F; return KBD;                                       // KBDSTROBE

    case 0xC020:                                                                // TAPEOUT (shall we listen it ? - try SAVE from applesoft)
    case 0xC030:                                                                // SPEAKER
    case 0xC033: playSound(); break;                                            // apple invader uses $C033 to output sound !

    case 0xC050: TEXT  = false; break;                                          // Graphics
    case 0xC051: TEXT  = true;  break;                                          // Text
    case 0xC052: MIXED = false; break;                                          // Mixed off
    case 0xC053: MIXED = true;  break;                                          // Mixed on
    case 0xC054: PAGE2 = false; break;                                          // PAGE2 off
    case 0xC055: PAGE2 = true;  break;                                          // PAGE2 on
    case 0xC056: HIRES = false; break;                                          // HiRes off
    case 0xC057: HIRES = true;  break;                                          // HiRes on

    case 0xC061: return PB0;                                                    // Push Button 0
    case 0xC062: return PB1;                                                    // Push Button 1
    case 0xC063: return PB2;                                                    // Push Button 2
    case 0xC064: return readPaddle(0);                                          // Paddle 0
    case 0xC065: return readPaddle(1);                                          // Paddle 1

    case 0xC070: resetPaddles(); break;                                         // paddle timer RST

    case 0xC080:                                                                // LANGUAGE CARD :
    case 0xC084: LCBK2 = 1; LCRD = 1; LCWR = 0;      LCWFF = 0;    break;       // LC2RD
    case 0xC081:
    case 0xC085: LCBK2 = 1; LCRD = 0; LCWR |= LCWFF; LCWFF = !WRT; break;       // LC2WR
    case 0xC082:
    case 0xC086: LCBK2 = 1; LCRD = 0; LCWR = 0;      LCWFF = 0;    break;       // ROMONLY2
    case 0xC083:
    case 0xC087: LCBK2 = 1; LCRD = 1; LCWR |= LCWFF; LCWFF = !WRT; break;       // LC2RW
    case 0xC088:
    case 0xC08C: LCBK2 = 0; LCRD = 1; LCWR = 0;      LCWFF = 0;    break;       // LC1RD
    case 0xC089:
    case 0xC08D: LCBK2 = 0; LCRD = 0; LCWR |= LCWFF; LCWFF = !WRT; break;       // LC1WR
    case 0xC08A:
    case 0xC08E: LCBK2 = 0; LCRD = 0; LCWR = 0;      LCWFF = 0;    break;       // ROMONLY1
    case 0xC08B:
    case 0xC08F: LCBK2 = 0; LCRD = 1; LCWR |= LCWFF; LCWFF = !WRT; break;       // LC1RW

        case 0xC0E0:
        case 0xC0E1:
        case 0xC0E2:
        case 0xC0E3:
        case 0xC0E4:
    case 0xC0E5:
        case 0xC0E6:
        case 0xC0E7: stepMotor(address); break;                                     // MOVE DRIVE HEAD

    case 0xCFFF:
    case 0xC0E8: disk[curDrv].motorOn = false; break;                           // MOTOROFF
    case 0xC0E9: disk[curDrv].motorOn = true;  break;                           // MOTORON

    case 0xC0EA: setDrv(0); break;                                              // DRIVE0EN
    case 0xC0EB: setDrv(1); break;                                              // DRIVE1EN

    case 0xC0EC:                                                                // Shift Data Latch
        if (disk[curDrv].writeMode)                                               // writting
            { if (disk[curDrv].mounted && !disk[curDrv].readOnly) { disk[curDrv].data[disk[curDrv].track*0x1A00+disk[curDrv].nibble]=dLatch; disk[curDrv].dirty = true; } }// good luck gcc
        else {                                                                    // reading
            apple2_machine->disk_reads++;
            dLatch = disk[curDrv].mounted ? disk[curDrv].data[disk[curDrv].track*0x1A00+disk[curDrv].nibble] : 0xff;
        }
        disk[curDrv].nibble = (disk[curDrv].nibble + 1) % 0x1A00;                 // turn floppy of 1 nibble
        return dLatch;

    case 0xC0ED: dLatch = value; break;                                         // Load Data Latch

    case 0xC0EE:                                                                // latch for READ
        disk[curDrv].writeMode = false;
        return disk[curDrv].readOnly ? 0x80 : 0;                                  // check protection

    case 0xC0EF: disk[curDrv].writeMode = true; break;                          // latch for WRITE
    }
    return ticks % 0xFF;                                                          // catch all, gives a 'floating' value
}


//======================================================================= MEMORY
// these two functions are imported into puce6502.c

uint8_t readMem(uint16_t address) {
    if (address < RAMSIZE)
        return ram[address];                                                        // RAM

    if (address >= ROMSTART) {
        if (!LCRD)
            return rom[address - ROMSTART];                                           // ROM

        if (LCBK2 && (address < 0xE000))
            return bk2[address - BK2START];                                           // BK2

        return lgc[address - LGCSTART];                                             // LC
    }

    if ((address & 0xFF00) == SL6START)
        return sl6[address - SL6START];                                             // disk][

    if ((address & 0xF000) == 0xC000)
        return softSwitches(address, 0, false);                                     // Soft Switches

    return ticks & 0xFF;                                                          // catch all, gives a 'floating' value
}


void writeMem(uint16_t address, uint8_t value) {
    if (address < RAMSIZE) {
        ram[address] = value;                                                       // RAM
        return;
    }

    if (LCWR && (address >= ROMSTART)) {
        if (LCBK2 && (address < 0xE000)) {
            bk2[address - BK2START] = value;                                          // BK2
            return;
        }
        lgc[address - LGCSTART] = value;                                            // LC
        return;
    }

    if ((address & 0xF000) == 0xC000) {
        softSwitches(address, value, true);                                         // Soft Switches
        return;
    }
}
