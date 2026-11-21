// Windows 64-bit loader for the Linux Attila binary: shared declarations.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Every function the Linux code calls directly uses the System V convention.
#define SYSV __attribute__((sysv_abi))

// One entry per function or data object offered to the Linux code. Entries land in a table the linker keeps together.
typedef struct { const char *name; void *ptr; } ShimEntry;

#define SHIM_REG(sym, p) \
    static const ShimEntry shimreg_##sym __attribute__((section(".shimtab$b"), used)) = {#sym, (void *)(p)}

// Define a shim: SHIM(int, close, (int fd)) { ... }
#define SHIM(ret, sym, params) \
    SYSV ret sh_##sym params; \
    SHIM_REG(sym, sh_##sym); \
    SYSV ret sh_##sym params

const ShimEntry *shim_find(const char *name);
int shim_debug(void);

// loader.c
void log_line(const char *fmt, ...);
void log_go_sync(void);   // before the program ends: write what is waiting, then write lines at once
void fatal(const char *fmt, ...);

typedef struct {
    const char *name;
    uint8_t *base;      // load bias: 0 for the fixed-address game file
    const void *phdr;
    int phnum;
    uint8_t *tls_image;
    size_t tls_filesz, tls_memsz, tls_align;
    int tls_module;
} LoadedModule;

int loader_module_count(void);
const LoadedModule *loader_module(int i);
void *loader_lookup(const char *name);
void *tls_block_for(int module_id);
