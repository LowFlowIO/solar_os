#pragma once
#include "solar_os_native_app_abi.h"
#include "solar_os_native_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <setjmp.h>

typedef struct tiny_file tiny_file;
extern const solar_os_native_foreground_api_v1_t *tiny_app;
extern const solar_os_native_memory_api_v1_t *tiny_memory;
extern const solar_os_native_host_api_v1_t *tiny_host;
extern jmp_buf tiny_trap;
extern const char *tiny_failure;
void *tiny_malloc(size_t size);
void *tiny_calloc(size_t count, size_t size);
void tiny_free(void *pointer);
int tiny_cleanup(void);
_Noreturn void tiny_abort(void);
_Noreturn void tiny_exit(int status);
_Noreturn void tiny_assert(const char *file, int line);
char *tiny_strdup(const char *text);
char *tiny_strcpy(char *out, const char *in);
int tiny_strcmp(const char *a, const char *b);
int tiny_atoi(const char *text);
tiny_file *tiny_fopen(const char *path, const char *mode);
int tiny_fclose(tiny_file *file);
int tiny_fseek(tiny_file *file, int64_t offset, int whence);
int64_t tiny_ftell(tiny_file *file);
size_t tiny_fread(void *buffer, size_t size, size_t count, tiny_file *file);
size_t tiny_fwrite(const void *buffer, size_t size, size_t count, tiny_file *file);
int tiny_fprintf(void *file, const char *format, ...);
int tiny_fputc(int ch, void *file);
int tiny_putchar(int ch);
int tiny_fflush(void *file);
void tiny_perror(const char *text);
int tiny_usleep(unsigned usecs);
int tiny_clock_gettime(int id, struct timespec *time);
time_t tiny_time(time_t *out);
struct tm *tiny_gmtime_r(const time_t *time, struct tm *out);
ssize_t tiny_write(int fd, const void *buffer, size_t length);
void tiny_clock_reset(void);
void tiny_clock_pause(uint64_t elapsed);
int tiny_save_disks(void);
void tiny_report(const char *text);

#ifndef TINY386_PORT_IMPLEMENTATION
#define malloc tiny_malloc
#define calloc tiny_calloc
#define free tiny_free
#define strdup tiny_strdup
#define strcpy tiny_strcpy
#define strcmp tiny_strcmp
#define atoi tiny_atoi
#define FILE tiny_file
#define fopen tiny_fopen
#define fclose tiny_fclose
#define fseek tiny_fseek
#define fseeko tiny_fseek
#define ftell tiny_ftell
#define ftello tiny_ftell
#define fread tiny_fread
#define fwrite tiny_fwrite
#define fprintf tiny_fprintf
#define fputc tiny_fputc
#define putchar tiny_putchar
#define fflush tiny_fflush
#define perror tiny_perror
#define abort tiny_abort
#define exit tiny_exit
#define usleep tiny_usleep
#define clock_gettime tiny_clock_gettime
#define time tiny_time
#define gmtime_r tiny_gmtime_r
#define write tiny_write
#undef assert
#define assert(test) ((test) ? (void)0 : tiny_assert(__FILE__, __LINE__))
#undef errno
#define errno EIO
#undef stdin
#undef stdout
#undef stderr
#define stdin ((void *)0)
#define stdout ((void *)0)
#define stderr ((void *)0)
#endif
