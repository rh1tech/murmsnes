/* APU on Core 1 - Parallel SPC700 emulation
 *
 * This module runs the SPC700 APU on Core 1 in parallel with the 65816 CPU on Core 0.
 * 
 * Architecture:
 * - Core 0 runs CPU and updates target cycle count atomically
 * - Core 1 runs APU to catch up whenever it has spare cycles
 * - No blocking synchronization needed for normal operation
 * - Port reads/writes use the existing atomic port buffers
 */

#ifdef PICO_ON_DEVICE

#include "apu_core1.h"
#include "apu.h"
#include "spc700.h"
#include "cpuexec.h"
#include "soundux.h"
#include "pico.h"
#include "hardware/sync.h"
#include <string.h>

/* Shared state between cores - aligned for atomic access */
volatile int32_t __attribute__((aligned(4))) apu_target_cycles = 0;
volatile bool apu_core1_enabled = false;

void apu_core1_init(void)
{
    apu_target_cycles = 0;
    apu_core1_enabled = true;
    __dmb();
}

/* Core 0 calls this to update target - non-blocking */
void __not_in_flash_func(apu_core1_set_target_cycles)(int32_t target)
{
    apu_target_cycles = target;
}

/* Run APU until caught up - called from Core 1 render loop */
void __not_in_flash_func(apu_core1_run_batch)(void)
{
    if (!apu_core1_enabled || !IAPU.APUExecuting) return;
    
    int32_t target = apu_target_cycles;
    
    /* Run APU until we catch up to target - no batch limit! 
     * Core 1 has spare cycles during HDMI blanking, use them all.
     * The APU typically runs ~21 CPU cycles per instruction.
     */
    while (APU.Cycles < target) {
        APUExecute();
    }
}

/* Check if APU has caught up to target */
bool __not_in_flash_func(apu_core1_is_caught_up)(void)
{
    return APU.Cycles >= apu_target_cycles;
}

#endif /* PICO_ON_DEVICE */
