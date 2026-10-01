#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32S3)
/* Eight BT.601 pixels. Signed 40-bit QACC retains the exact scalar rounding.
 * Inputs and output are 16-byte aligned. No persistent workspace or tables in
 * SRAM. The Xtensa RTOS preserves PIE state across task switches. */
    .section .rodata.mpeg_rgb_constants,"a"
    .balign 16
.Ly:     .short 298,298,298,298,298,298,298,298
.Lcr:    .short 409,409,409,409,409,409,409,409
.Lcbg:   .short -100,-100,-100,-100,-100,-100,-100,-100
.Lcrg:   .short -208,-208,-208,-208,-208,-208,-208,-208
.Lcbb:   .short 516,516,516,516,516,516,516,516
.Lround: .short 128,128,128,128,128,128,128,128
.Lone:   .short 1,1,1,1,1,1,1,1
.Lmax:   .short 255,255,255,255,255,255,255,255
.Lrmask: .short 248,248,248,248,248,248,248,248
.Lgmask: .short 252,252,252,252,252,252,252,252
.Lrscale:.short 256,256,256,256,256,256,256,256
.Lgscale:.short 8,8,8,8,8,8,8,8
.Lbytes: .long 0x00ff00ff,0x00ff00ff,0x00ff00ff,0x00ff00ff

    .text
    .balign 4
    .global solar_os_mpeg_rgb565_s3
    .type solar_os_mpeg_rgb565_s3,@function
solar_os_mpeg_rgb565_s3:
    entry a1, 16
    mov a8, a2
    mov a9, a3
    movi a10, 8
    ee.vld.128.ip q0, a8, 16
    ee.vld.128.ip q1, a8, 16
    ee.vld.128.ip q2, a8, 16
    ee.zero.q q7

    ee.zero.qacc
    movi a8, .Ly
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q0, q3
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q2, q3
    movi a8, .Lround
    ee.vld.128.ip q3, a8, 16
    ee.vld.128.ip q4, a8, 16
    ee.vmulas.s16.qacc q3, q4
    ee.srcmb.s16.qacc q5, a10, 0
    ee.vmax.s16 q5, q5, q7
    movi a8, .Lmax
    ee.vld.128.ip q3, a8, 16
    ee.vmin.s16 q5, q5, q3

    ee.zero.qacc
    movi a8, .Ly
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q0, q3
    movi a8, .Lcbg
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q1, q3
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q2, q3
    movi a8, .Lround
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q3, q4
    ee.srcmb.s16.qacc q6, a10, 0
    ee.vmax.s16 q6, q6, q7
    movi a8, .Lmax
    ee.vld.128.ip q3, a8, 16
    ee.vmin.s16 q6, q6, q3

    ee.zero.qacc
    movi a8, .Ly
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q0, q3
    movi a8, .Lcbb
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q1, q3
    movi a8, .Lround
    ee.vld.128.ip q3, a8, 16
    ee.vmulas.s16.qacc q3, q4
    ee.srcmb.s16.qacc q2, a10, 0
    ee.vmax.s16 q2, q2, q7
    movi a8, .Lmax
    ee.vld.128.ip q3, a8, 16
    ee.vmin.s16 q2, q2, q3

    movi a10, 0
    wsr.sar a10
    movi a8, .Lrmask
    ee.vld.128.ip q3, a8, 16
    ee.andq q5, q5, q3
    movi a8, .Lrscale
    ee.vld.128.ip q3, a8, 16
    ee.vmul.u16 q5, q5, q3
    movi a8, .Lgmask
    ee.vld.128.ip q3, a8, 16
    ee.andq q6, q6, q3
    movi a8, .Lgscale
    ee.vld.128.ip q3, a8, 16
    ee.vmul.u16 q6, q6, q3
    movi a10, 3
    wsr.sar a10
    ee.vmul.u16 q2, q2, q4
    ee.orq q5, q5, q6
    ee.orq q5, q5, q2

    /* Byte-swap each RGB565 lane for the display's wire order. */
    movi a10, 8
    wsr.sar a10
    movi a8, .Lbytes
    ee.vld.128.ip q3, a8, 16
    ee.andq q6, q5, q3
    ee.vsl.32 q6, q6
    ee.vsr.32 q5, q5
    ee.andq q5, q5, q3
    ee.orq q5, q5, q6
    ee.vst.128.ip q5, a9, 16
    retw.n
    .size solar_os_mpeg_rgb565_s3, .-solar_os_mpeg_rgb565_s3
#endif
