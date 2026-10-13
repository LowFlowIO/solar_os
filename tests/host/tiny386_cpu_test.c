/* Compare complete guest-visible state with the unfused interpreter. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef CORE_SOURCE
#define CORE_SOURCE "vendor/i386.c"
#endif
#include CORE_SOURCE

static unsigned scenario, io_reads, io_writes;
static uint8_t io_code[64];
static uint8_t *mapped_ram;

static uint8_t code_io_read(void *opaque, uword address)
{
    (void)opaque;
    assert(address >= 0xa0000 && address < 0xa0040);
    ++io_reads;
    return io_code[address - 0xa0000];
}

static uint8_t data_io_read(void *opaque, uword address)
{
    (void)opaque;
    assert(address >= 0xa0000 && address < 0xc0000);
    ++io_reads;
    return mapped_ram[address];
}

static void data_io_write(void *opaque, uword address, uint8_t value)
{
    (void)opaque;
    assert(address >= 0xa0000 && address < 0xc0000);
    ++io_writes;
    mapped_ram[address] = value;
}

static void snapshot(CPUI386 *cpu, bool result)
{
    CPUI386_State state;
    cpui386_get_state(cpu, &state);
    printf("%u %u %ld %08x %08x %08x %08x %u %08x %u", scenario++, result,
           cpu->cycle, cpu->ip, cpu->next_ip, state.flags, cpu->cc.mask,
           cpu->excno, cpu->cr2, io_reads);
    printf(" %u", io_writes);
    for (unsigned i = 0; i < 8; ++i) printf(" %08x", state.gpr[i]);
    puts("");
}

static CPUI386 *create_cpu(uint8_t *ram, uword address, bool code16)
{
    CPU_CB *callbacks;
    CPUI386 *cpu = cpui386_new(3, (char *)ram, 2 * 1024 * 1024, &callbacks);
    assert(cpu);
    cpu->seg[SEG_CS].base = code16 ? address & ~0xffffU : 0;
    cpu->seg[SEG_CS].sel = 8;
    cpu->seg[SEG_CS].limit = 0xffffffff;
    cpu->ip = cpu->next_ip = code16 ? address & 0xffff : address;
    cpu->code16 = code16;
    cpu->seg[SEG_CS].flags = code16 ? 0 : SEG_D_BIT;
    io_reads = io_writes = 0;
    return cpu;
}

static void regular_cases(void)
{
    const uint32_t values[] = {0, 1, 2, 0x8000, 0xffff, 0xaabb0001, 0x80000000};
    uint8_t *ram = calloc(1, 2 * 1024 * 1024);
    assert(ram);
    for (unsigned protected_mode = 0; protected_mode < 2; ++protected_mode)
      for (unsigned wide = 0; wide < 2; ++wide)
        for (unsigned reg = 0; reg < 8; ++reg)
            for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v)
                for (unsigned branch = 0; branch < 4; ++branch)
                    for (unsigned budget = 1; budget <= 4; ++budget) {
                        memset(ram + 0x10000, 0x90, 64);
                        unsigned length = 0;
                        if (wide) ram[0x10000 + length++] = 0x66;
                        ram[0x10000 + length++] = 0x48 + reg;
                        ram[0x10000 + length++] = branch & 1 ? 0x75 : 0x74;
                        ram[0x10000 + length++] = branch & 2 ? 2 : -(int)(wide ? 4 : 3);
                        CPUI386 *cpu = create_cpu(ram, 0x10000, !protected_mode);
                        cpu->cr0 = protected_mode ? 1 : 0;
                        cpui386_set_gpr(cpu, reg, values[v]);
                        cpu->flags |= v & 1 ? CF : 0;
                        snapshot(cpu, cpu_exec1(cpu, budget));
                        cpui386_delete(cpu);
                    }
    /* Fall through unrelated opcodes and debugger single-step mode. */
    const uint8_t following[] = {0x90, 0x72, 0x73, 0x66};
    for (unsigned i = 0; i < sizeof(following); ++i) {
        memset(ram + 0x10000, 0x90, 64);
        ram[0x10000] = 0x49; ram[0x10001] = following[i]; ram[0x10002] = 0x90;
        CPUI386 *cpu = create_cpu(ram, 0x10000, true);
        cpui386_set_gpr(cpu, 1, 2);
        snapshot(cpu, cpu_exec1(cpu, 2)); cpui386_delete(cpu);
    }
    ram[0x10000] = 0x49; ram[0x10001] = 0x75; ram[0x10002] = 0xfd;
    CPUI386 *cpu = create_cpu(ram, 0x10000, true);
    cpui386_set_gpr(cpu, 1, 2); cpu->flags |= TF;
    snapshot(cpu, cpu_exec1(cpu, 2)); cpui386_delete(cpu);
    /* Modified guest code must be decoded again on the next execution. */
    cpu = create_cpu(ram, 0x10000, true); cpui386_set_gpr(cpu, 1, 3);
    snapshot(cpu, cpu_exec1(cpu, 2));
    ram[0x10001] = 0x74;
    snapshot(cpu, cpu_exec1(cpu, 2)); cpui386_delete(cpu);
    free(ram);
}

static void boundary_cases(void)
{
    uint8_t *ram = calloc(1, 2 * 1024 * 1024);
    assert(ram);
    for (unsigned address = 0xffed; address <= 0x10001; ++address) {
        memset(ram + 0xffe0, 0x90, 64);
        ram[address] = 0x49; ram[address + 1] = 0x75; ram[address + 2] = 0xfd;
        CPUI386 *cpu = create_cpu(ram, address, true); cpui386_set_gpr(cpu, 1, 2);
        snapshot(cpu, cpu_exec1(cpu, 2)); cpui386_delete(cpu);
    }
    /* Adjacent virtual code pages map to non-adjacent physical pages. The
     * missing-page variant verifies the fault position and instruction count. */
    for (unsigned missing = 0; missing < 2; ++missing)
        for (unsigned offset = 0xff0; offset < 0x1000; ++offset) {
            memset(ram + 0x20000, 0x90, 4096);
            memset(ram + 0x40000, 0x90, 4096);
            *(uint32_t *)(ram + 0x1000) = 0x2003;
            *(uint32_t *)(ram + 0x2000 + 16 * 4) = 0x20003;
            *(uint32_t *)(ram + 0x2000 + 17 * 4) = missing ? 0 : 0x40003;
            const uint8_t bytes[] = {0x49, 0x75, 0xfd};
            for (unsigned i = 0; i < sizeof(bytes); ++i) {
                unsigned position = offset + i;
                ram[(position < 4096 ? 0x20000 : 0x40000) + (position & 4095)] = bytes[i];
            }
            CPUI386 *cpu = create_cpu(ram, 0x10000 + offset, false);
            cpu->cr0 = CR0_PG | 1; cpu->cr3 = 0x1000;
            cpui386_set_gpr(cpu, 1, 2);
            snapshot(cpu, cpu_exec1(cpu, 2)); cpui386_delete(cpu);
        }
    /* Code served by a VGA memory callback must retain byte-by-byte reads. */
    memset(io_code, 0x90, sizeof(io_code));
    io_code[0] = 0x49; io_code[1] = 0x75; io_code[2] = 0xfd;
    CPUI386 *cpu = create_cpu(ram, 0xa0000, false);
    cpu->cb.iomem_read8 = code_io_read; cpui386_set_gpr(cpu, 1, 2);
    snapshot(cpu, cpu_exec1(cpu, 2));
    assert(io_reads == 3); cpui386_delete(cpu);
    free(ram);
}

static void copy_cases(void)
{
    uint8_t *ram = calloc(1, 2 * 1024 * 1024);
    assert(ram);
    const unsigned counts[] = {0, 1, 7, 8, 17, 1000, 5000};
    const unsigned gaps[] = {0, 1, 4, 16, 8192};
    for (unsigned width = 1; width <= 4; width *= 2)
        for (unsigned backwards = 0; backwards < 2; ++backwards)
            for (unsigned unaligned = 0; unaligned < 2; ++unaligned)
                for (unsigned gap = 0; gap < sizeof(gaps) / sizeof(gaps[0]); ++gap)
                    for (unsigned c = 0; c < sizeof(counts) / sizeof(counts[0]); ++c) {
                        for (unsigned i = 0x10000; i < 0x30000; ++i) ram[i] = i * 13 + (i >> 8);
                        unsigned length = 0;
                        ram[0x8000 + length++] = 0xf3;
                        if (width == 2) ram[0x8000 + length++] = 0x66;
                        ram[0x8000 + length++] = width == 1 ? 0xa4 : 0xa5;
                        CPUI386 *cpu = create_cpu(ram, 0x8000, false);
                        if (backwards) cpu->flags |= DF;
                        cpui386_set_gpr(cpu, 1, counts[c]);
                        cpui386_set_gpr(cpu, 6, 0x18000 + unaligned);
                        cpui386_set_gpr(cpu, 7, 0x18000 + unaligned + gaps[gap]);
                        snapshot(cpu, cpu_exec1(cpu, 1));
                        uint32_t checksum = 2166136261U;
                        for (unsigned i = 0x10000; i < 0x30000; ++i) checksum = (checksum ^ ram[i]) * 16777619U;
                        printf("copy %08x\n", checksum);
                        cpui386_delete(cpu);
                    }
    /* A later unmapped destination page must fault after the same partial
     * copy, with the same remaining count and source/destination indices. */
    for (unsigned missing = 0; missing < 2; ++missing) {
        memset(ram, 0, 2 * 1024 * 1024);
        *(uint32_t *)(ram + 0x1000) = 0x2003;
        *(uint32_t *)(ram + 0x2000 + 16 * 4) = 0x20003;
        *(uint32_t *)(ram + 0x2000 + 48 * 4) = 0x50003;
        *(uint32_t *)(ram + 0x2000 + 49 * 4) = 0x70003;
        *(uint32_t *)(ram + 0x2000 + 64 * 4) = 0x60003;
        *(uint32_t *)(ram + 0x2000 + 65 * 4) = missing ? 0 : 0x80003;
        ram[0x20000] = 0xf3; ram[0x20001] = 0xa4;
        memset(ram + 0x50ff0, 0x5a, 16); memset(ram + 0x70000, 0xa5, 16);
        CPUI386 *cpu = create_cpu(ram, 0x10000, false);
        cpu->cr0 = CR0_PG | 1; cpu->cr3 = 0x1000;
        cpu->seg[SEG_DS].sel = cpu->seg[SEG_ES].sel = 0x10;
        cpu->seg[SEG_DS].limit = cpu->seg[SEG_ES].limit = 0xffffffff;
        cpui386_set_gpr(cpu, 1, 32);
        cpui386_set_gpr(cpu, 6, 0x30ff0); cpui386_set_gpr(cpu, 7, 0x40ff0);
        bool result = cpu_exec1(cpu, 1);
        assert(result == !missing);
        for (unsigned i = 0; i < 16; ++i) assert(ram[0x60ff0 + i] == 0x5a);
        for (unsigned i = 0; i < 16; ++i) assert(ram[0x80000 + i] == (missing ? 0 : 0xa5));
        snapshot(cpu, result); cpui386_delete(cpu);
    }
    /* VGA memory callbacks must still run once per copied byte. */
    for (unsigned io_destination = 0; io_destination < 2; ++io_destination) {
        memset(ram, 0, 2 * 1024 * 1024); mapped_ram = ram;
        unsigned source = io_destination ? 0x80000 : 0xa0000;
        unsigned destination = io_destination ? 0xa0000 : 0x80000;
        memset(ram + source, 0x5a, 512);
        ram[0x8000] = 0xf3; ram[0x8001] = 0xa4;
        CPUI386 *cpu = create_cpu(ram, 0x8000, false);
        cpu->cb.iomem_read8 = data_io_read; cpu->cb.iomem_write8 = data_io_write;
        cpui386_set_gpr(cpu, 1, 512);
        cpui386_set_gpr(cpu, 6, source); cpui386_set_gpr(cpu, 7, destination);
        assert(cpu_exec1(cpu, 1));
        assert(io_destination ? io_writes == 512 : io_reads == 512);
        for (unsigned i = 0; i < 512; ++i) assert(ram[destination + i] == 0x5a);
        snapshot(cpu, true); cpui386_delete(cpu);
    }
    free(ram);
}

int main(void)
{
    regular_cases(); boundary_cases(); copy_cases();
    return 0;
}
