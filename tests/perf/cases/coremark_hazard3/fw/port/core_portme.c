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

Modified by Kalyan Sriram for Clotho, 2026: adapted from barebones/core_portme.c at
eembc/coremark@1f483d5b8316 for bare-metal RV32 cores (cycle-counter timing,
output through the testbench).
*/
/* CoreMark port glue for bare-metal RV32. Generic RISC-V (mcycle CSR). */
#include "coremark.h"

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

static CORE_TICKS start_time_val, stop_time_val;

static inline ee_u32 read_mcycle(void)
{
    ee_u32 c;
    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

void start_time(void) { start_time_val = read_mcycle(); }
void stop_time(void)  { stop_time_val  = read_mcycle(); }

CORE_TICKS get_time(void)
{
    return (CORE_TICKS)(stop_time_val - start_time_val);
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    return ((secs_ret)ticks) / (secs_ret)EE_TICKS_PER_SEC;
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    /* Some RISC-V cores reset mcountinhibit with the cycle counter frozen;
       clear it so mcycle advances. CSR 0x320 = mcountinhibit. */
    __asm__ volatile("csrw 0x320, zero");
    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
        ee_printf("ERROR! ee_ptr_int size mismatch!\n");
    if (sizeof(ee_u32) != 4)
        ee_printf("ERROR! ee_u32 size mismatch!\n");
    p->portable_id = 1;
}

void portable_fini(core_portable *p)
{
    p->portable_id = 0;
    /* Explicit, deterministic cycle count for the Clotho perf harness. */
    ee_printf("MCYCLES: %u\n", (unsigned)(stop_time_val - start_time_val));
}
