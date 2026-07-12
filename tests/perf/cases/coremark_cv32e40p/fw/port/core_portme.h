/*
Copyright 2018 Embedded Microprocessor Benchmark Consortium (EEMBC)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Original Author: Shay Gal-on

Modified by Kalyan Sriram for Clotho, 2026: adapted from barebones/core_portme.h at
eembc/coremark@1f483d5b8316 for bare-metal RV32 cores (cycle-counter timing,
output through the testbench).
*/
/* CoreMark port for bare-metal RISC-V (RV32) cores under Clotho.
   Generic across cores: timing via the mcycle CSR, output via newlib printf
   (the per-core BSP routes _write to the testbench's char-out register).
   Nothing here is cv32e40p-specific — reuse for ibex/picorv32/etc. */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stdint.h>
#include <stddef.h>

/* We use newlib printf (routed by the BSP), no float, no host time.h. */
#define HAS_FLOAT   0
#define HAS_TIME_H  0
#define USE_CLOCK   0
#define HAS_STDIO   0
#define HAS_PRINTF  0

#define COMPILER_VERSION "GCC" __VERSION__
#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS  FLAGS_STR
#endif
#define MEM_LOCATION "STATIC"

typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef uintptr_t      ee_ptr_int;
typedef size_t         ee_size_t;
#ifndef NULL
#define NULL ((void *)0)
#endif

/* mcycle is read as a 32-bit tick; keep the benchmark under ~4e9 cycles. */
typedef ee_u32 CORE_TICKS;

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD  MEM_STATIC
#define MULTITHREAD 1
#define USE_PTHREAD 0
#define USE_FORK    0
#define USE_SOCKET  0

#define MAIN_HAS_NOARGC   1
#define MAIN_HAS_NORETURN 0

extern ee_u32 default_num_contexts;

typedef struct CORE_PORTABLE_S {
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

/* Self-contained ee_printf (cm/barebones/ee_printf.c) routed to the TB. */
int ee_printf(const char *fmt, ...);

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#define PERFORMANCE_RUN 1
#endif

/* Nominal only — CoreMark/MHz is frequency-independent (iterations per
   million cycles), and we also print the raw cycle count for benchmarking. */
#define EE_TICKS_PER_SEC 1

#endif /* CORE_PORTME_H */
