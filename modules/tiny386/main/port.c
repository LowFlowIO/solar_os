#define TINY386_PORT_IMPLEMENTATION
#include "port.h"
#include "vendor/pc.h"

const solar_os_native_foreground_api_v1_t *tiny_app;
const solar_os_native_memory_api_v1_t *tiny_memory;
const solar_os_native_host_api_v1_t *tiny_host;
jmp_buf tiny_trap;
const char *tiny_failure;
static void *allocations[96];
static uint64_t clock_origin, clock_paused;
struct tiny_file {
    char path[192];
    uint8_t *data;
    uint32_t size, offset;
    bool open, dirty;
};
static tiny_file files[4];
static char bios_log[160];
static unsigned bios_log_length;

void tiny_report(const char *text) { tiny_host->write_utf8(text, strlen(text)); }
_Noreturn void tiny_abort(void) { if (!tiny_failure) tiny_failure = "guest CPU or device failed"; longjmp(tiny_trap, 1); }
_Noreturn void tiny_exit(int status) { (void)status; tiny_abort(); }
_Noreturn void tiny_assert(const char *file, int line) { (void)file; (void)line; tiny_abort(); }
void *tiny_malloc(size_t size)
{
    for (unsigned i = 0; i < 96; ++i) if (!allocations[i]) {
        void *p = tiny_memory->alloc(size ? size : 1);
        if (!p) { tiny_failure = "not enough PSRAM"; tiny_abort(); }
        allocations[i] = p;
        return p;
    }
    tiny_failure = "allocation limit reached"; tiny_abort();
}
void *tiny_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) tiny_abort();
    void *p = tiny_malloc(count * size); memset(p, 0, count * size); return p;
}
void tiny_free(void *p)
{
    if (!p) return;
    for (unsigned i = 0; i < 96; ++i) if (allocations[i] == p) {
        allocations[i] = NULL; tiny_memory->free(p); return;
    }
    tiny_failure = "invalid guest allocation"; tiny_abort();
}
void tiny_cleanup(void)
{
    for (unsigned i = 0; i < 96; ++i) if (allocations[i]) {
        tiny_memory->free(allocations[i]); allocations[i] = NULL;
    }
    memset(files, 0, sizeof(files));
    bios_log_length = 0;
}
void *bigmalloc(size_t size) { return tiny_malloc(size); }
char *tiny_strcpy(char *out, const char *in) { memcpy(out, in, strlen(in) + 1); return out; }
char *tiny_strdup(const char *s) { return tiny_strcpy(tiny_malloc(strlen(s) + 1), s); }
int tiny_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
int tiny_atoi(const char *s) { int n = 0; while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0'; return n; }
tiny_file *tiny_fopen(const char *path, const char *mode)
{
    (void)mode;
    uint32_t size;
    if (!path || strlen(path) >= sizeof(files[0].path) ||
        tiny_app->file_size(path, &size) || !size || size > 2880U * 1024U) return NULL;
    for (unsigned i = 0; i < 4; ++i) if (!files[i].open) {
        tiny_file *f = &files[i];
        memset(f, 0, sizeof(*f));
        tiny_strcpy(f->path, path); f->data = tiny_malloc(size); f->size = size;
        for (uint32_t off = 0; off < size;) {
            size_t n = size - off, got = 0;
            if (n > 65536) n = 65536;
            if (tiny_app->read_file(path, off, f->data + off, n, &got) || got != n) {
                tiny_free(f->data); memset(f, 0, sizeof(*f)); return NULL;
            }
            off += (uint32_t)n;
        }
        f->open = true; return f;
    }
    return NULL;
}
int tiny_fclose(tiny_file *f)
{
    if (!f || !f->open) return -1;
    if (f->dirty && tiny_app->replace_file(f->path, f->data, f->size)) return -1;
    tiny_free(f->data); memset(f, 0, sizeof(*f)); return 0;
}
int tiny_fseek(tiny_file *f, int64_t offset, int whence)
{
    if (!f || !f->open) return -1;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->offset : whence == SEEK_END ? f->size : -1;
    if (base < 0 || offset < -base || offset > (int64_t)f->size - base) return -1;
    f->offset = (uint32_t)(base + offset); return 0;
}
int64_t tiny_ftell(tiny_file *f) { return f && f->open ? f->offset : -1; }
size_t tiny_fread(void *out, size_t size, size_t count, tiny_file *f)
{
    if (!f || !f->open || !size || count > SIZE_MAX / size) return 0;
    size_t n = count * size;
    if (n > f->size - f->offset) n = (f->size - f->offset) / size * size;
    memcpy(out, f->data + f->offset, n); f->offset += (uint32_t)n; return n / size;
}
size_t tiny_fwrite(const void *in, size_t size, size_t count, tiny_file *f)
{
    if (!f || !f->open || !size || count > SIZE_MAX / size || count * size > f->size - f->offset) return 0;
    size_t n = count * size;
    memcpy(f->data + f->offset, in, n); f->offset += (uint32_t)n;
    f->dirty |= n != 0; return count;
}
int tiny_save_disks(void)
{
    int result = 0;
    for (unsigned i = 0; i < 4; ++i) if (files[i].open && files[i].dirty) {
        int e = tiny_app->replace_file(files[i].path, files[i].data, files[i].size);
        if (e) result = e; else files[i].dirty = false;
    }
    return result;
}
int tiny_fprintf(void *file, const char *format, ...) { (void)file; (void)format; return 0; }
int tiny_fputc(int ch, void *file) { (void)file; (void)ch; return ch; }
int tiny_putchar(int ch)
{
    if (ch == '\n' || bios_log_length + 1 >= sizeof(bios_log)) {
        bios_log[bios_log_length] = 0;
        if (bios_log_length) tiny_report(bios_log);
        bios_log_length = 0;
    } else if (ch >= 32 && ch < 127) bios_log[bios_log_length++] = (char)ch;
    return ch;
}
int tiny_fflush(void *file) { (void)file; return 0; }
void tiny_perror(const char *text) { tiny_failure = text; }
int tiny_usleep(unsigned usecs) { (void)usecs; return 0; }
void tiny_clock_reset(void) { clock_origin = tiny_app->time_us(); clock_paused = 0; }
void tiny_clock_pause(uint64_t elapsed) { clock_paused += elapsed; }
uint32_t get_uticks(void) { return (uint32_t)(tiny_app->time_us() - clock_origin - clock_paused); }
int tiny_clock_gettime(int id, struct timespec *t)
{
    (void)id; uint64_t us = tiny_app->time_us() - clock_origin - clock_paused;
    t->tv_sec = (time_t)(us / 1000000); t->tv_nsec = (long)((us % 1000000) * 1000); return 0;
}
time_t tiny_time(time_t *out) { time_t t = 946684800 + get_uticks() / 1000000; if (out) *out = t; return t; }
struct tm *tiny_gmtime_r(const time_t *t, struct tm *out)
{
    memset(out, 0, sizeof(*out)); out->tm_year = 100; out->tm_mday = 1;
    out->tm_hour = (int)(*t / 3600 % 24); out->tm_min = (int)(*t / 60 % 60); out->tm_sec = (int)(*t % 60); return out;
}
ssize_t tiny_write(int fd, const void *buffer, size_t n) { (void)fd; (void)buffer; return (ssize_t)n; }

/* Guest networking is absent. The board network remains owned by SolarOS. */
NE2000State *isa_ne2000_init(int b, int i, void *p, void (*irq)(void *, int, int)) { (void)b; (void)i; (void)p; (void)irq; return NULL; }
void ne2000_step(NE2000State *s) { (void)s; }
void ne2000_ioport_write(void *o, uint32_t a, uint32_t v) { (void)o; (void)a; (void)v; }
uint32_t ne2000_ioport_read(void *o, uint32_t a) { (void)o; (void)a; return 0xff; }
void ne2000_reset_ioport_write(void *o, uint32_t a, uint32_t v) { (void)o; (void)a; (void)v; }
uint32_t ne2000_reset_ioport_read(void *o, uint32_t a) { (void)o; (void)a; return 0xff; }
void ne2000_asic_ioport_write(void *o, uint32_t a, uint32_t v) { (void)o; (void)a; (void)v; }
uint32_t ne2000_asic_ioport_read(void *o, uint32_t a) { (void)o; (void)a; return 0xff; }
