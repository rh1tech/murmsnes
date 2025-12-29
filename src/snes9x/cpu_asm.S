/* This file is part of Snes9x. See LICENSE file. */
/* Assembly-optimized CPU functions for ARM Cortex-M33 (RP2350) */

.syntax unified
.cpu cortex-m33
.thumb

/* Put in time-critical section for best cache behavior */
.section .time_critical.snes9x_cpu_asm, "ax", %progbits

/* External symbols - these are global structs, not pointers */
.extern Memory
.extern CPU
.extern ICPU
.extern OpenBus
.extern S9xGetByte
.extern S9xGetWord

/* Memory map constants (from memmap.h) */
.equ MEMMAP_SHIFT, 12
.equ MEMMAP_MASK, 0xFFF

/* MAP_LAST constant - addresses >= this are direct memory pointers */
.equ MAP_LAST, 18

/* CMemory struct offsets (from memmap.h):
 * uint8_t *RAM;        // +0
 * uint8_t* ROM;        // +4
 * uint8_t *VRAM;       // +8
 * uint8_t* SRAM;       // +12
 * uint8_t* FillRAM;    // +16
 * uint8_t* C4RAM;      // +20
 * bool     HiROM;      // +24
 * bool     LoROM;      // +25
 * uint16_t SRAMMask;   // +26
 * uint8_t  SRAMSize;   // +28
 * uint8_t**Map;        // +32 (pointer to array)
 * SMapInfo*MapInfo;    // +36 (pointer to array)
 */
.equ MEMORY_OFF_MAP, 32
.equ MEMORY_OFF_MAPINFO, 36

/* SCPUState struct offsets (from snes9x.h):
 * uint32_t Flags;                  // +0
 * bool     BranchSkip;             // +4
 * bool     NMIActive;              // +5
 * uint8_t  IRQActive;              // +6
 * bool     WaitingForInterrupt;    // +7
 * bool     InDMA;                  // +8
 * uint8_t  WhichEvent;             // +9
 * uint8_t* PC;                     // +12 (aligned)
 * uint8_t* PCBase;                 // +16
 * uint8_t* PCAtOpcodeStart;        // +20
 * uint8_t* WaitAddress;            // +24
 * uint32_t WaitCounter;            // +28
 * long     Cycles;                 // +32
 */
.equ CPU_OFF_INDMA, 8
.equ CPU_OFF_CYCLES, 32

/* SMapInfo is a packed struct:
 * uint8_t Speed:5;
 * uint8_t Type:3;
 * Total size = 1 byte per entry
 */
.equ MAPINFO_ENTRY_SIZE, 1


/*
 * S9xGetByte_asm - Fast path memory read for direct-mapped ROM/RAM
 * 
 * This is called extremely frequently - every memory access from 65816 CPU.
 * The fast path handles direct memory (ROM/RAM) which is ~95% of accesses.
 * Special addresses (PPU, CPU, DSP registers) fall back to C.
 *
 * Input:  r0 = Address (24-bit SNES address)
 * Output: r0 = byte value
 * Clobbers: r1-r3
 */
.global S9xGetByte_asm
.type S9xGetByte_asm, %function
.thumb_func
S9xGetByte_asm:
    push    {r4, r5, lr}
    
    mov     r4, r0                      @ r4 = save Address
    
    /* block = (Address >> MEMMAP_SHIFT) & MEMMAP_MASK = Address >> 12 & 0xFFF */
    /* Since Address is 24-bit max, >> 12 gives max 0xFFF, no mask needed */
    lsr     r1, r0, #MEMMAP_SHIFT       @ r1 = block index (0-4095)
    
    /* GetAddress = Memory.Map[block] */
    ldr     r2, =Memory                 @ r2 = &Memory (struct address)
    ldr     r3, [r2, #MEMORY_OFF_MAP]   @ r3 = Memory.Map (pointer to array)
    ldr     r3, [r3, r1, lsl #2]        @ r3 = Memory.Map[block] (each entry is 4 bytes)
    
    /* Check if GetAddress >= MAP_LAST (direct memory access) */
    cmp     r3, #MAP_LAST
    blo     .Lgetbyte_slow_path
    
    /* === Fast path: direct memory access === */
    
    /* Check InDMA to decide on cycle update */
    ldr     r5, =CPU                    @ r5 = &CPU
    ldrb    r0, [r5, #CPU_OFF_INDMA]    @ r0 = CPU.InDMA
    cbnz    r0, .Lgetbyte_skip_cycles   @ if InDMA, skip cycle update
    
    /* CPU.Cycles += Memory.MapInfo[block].Speed */
    ldr     r0, [r2, #MEMORY_OFF_MAPINFO] @ r0 = Memory.MapInfo
    ldrb    r0, [r0, r1]                @ r0 = MapInfo[block] (1 byte entry)
    and     r0, r0, #0x1F               @ Speed is lower 5 bits
    
    ldr     r2, [r5, #CPU_OFF_CYCLES]   @ r2 = CPU.Cycles
    add     r2, r2, r0                  @ r2 = CPU.Cycles + Speed
    str     r2, [r5, #CPU_OFF_CYCLES]   @ CPU.Cycles = updated
    
.Lgetbyte_skip_cycles:
    /* return GetAddress[Address & 0xFFFF] */
    uxth    r0, r4                      @ r0 = Address & 0xFFFF (zero-extend halfword)
    ldrb    r0, [r3, r0]                @ r0 = GetAddress[offset]
    
    pop     {r4, r5, pc}

.Lgetbyte_slow_path:
    /* Fall back to C implementation for PPU/CPU/DSP/etc */
    mov     r0, r4                      @ restore Address
    bl      S9xGetByte                  @ call C version
    pop     {r4, r5, pc}

.size S9xGetByte_asm, . - S9xGetByte_asm


/*
 * S9xGetWord_asm - Fast path 16-bit memory read
 * 
 * Input:  r0 = Address (24-bit SNES address)
 * Output: r0 = 16-bit word value (little-endian)
 * Clobbers: r1-r3
 */
.global S9xGetWord_asm
.type S9xGetWord_asm, %function
.thumb_func
S9xGetWord_asm:
    push    {r4, r5, r6, lr}
    
    mov     r4, r0                      @ r4 = save Address
    
    /* Check for page boundary crossing at 0xFFF */
    /* If (Address & 0xFFF) == 0xFFF, the word spans two memory blocks */
    movw    r1, #0x0FFF
    and     r1, r0, r1
    movw    r2, #0x0FFF
    cmp     r1, r2
    beq     .Lgetword_slow_path         @ Page boundary, use C version
    
    /* block = Address >> 12 */
    lsr     r1, r0, #MEMMAP_SHIFT       @ r1 = block
    
    /* GetAddress = Memory.Map[block] */
    ldr     r2, =Memory
    ldr     r3, [r2, #MEMORY_OFF_MAP]
    ldr     r3, [r3, r1, lsl #2]        @ r3 = Memory.Map[block]
    
    /* Check if direct memory */
    cmp     r3, #MAP_LAST
    blo     .Lgetword_slow_path
    
    /* === Fast path === */
    
    /* Check InDMA */
    ldr     r5, =CPU
    ldrb    r0, [r5, #CPU_OFF_INDMA]
    cbnz    r0, .Lgetword_skip_cycles
    
    /* CPU.Cycles += Speed * 2 */
    ldr     r0, [r2, #MEMORY_OFF_MAPINFO]
    ldrb    r0, [r0, r1]                @ MapInfo[block]
    and     r0, r0, #0x1F               @ Speed
    lsl     r0, r0, #1                  @ Speed * 2
    
    ldr     r2, [r5, #CPU_OFF_CYCLES]
    add     r2, r2, r0
    str     r2, [r5, #CPU_OFF_CYCLES]
    
.Lgetword_skip_cycles:
    /* return *(uint16_t*)(GetAddress + (Address & 0xFFFF)) */
    uxth    r0, r4                      @ r0 = Address & 0xFFFF
    ldrh    r0, [r3, r0]                @ 16-bit load (Cortex-M33 supports unaligned)
    
    pop     {r4, r5, r6, pc}

.Lgetword_slow_path:
    mov     r0, r4                      @ restore Address
    bl      S9xGetWord                  @ call C version
    pop     {r4, r5, r6, pc}

.size S9xGetWord_asm, . - S9xGetWord_asm


/*
 * SICPU struct offsets (from cpuexec.h):
 * uint8_t*   UNUSED1;        // +0  (4 bytes)
 * const SOpcodes*  S9xOpcodes; // +4  (4 bytes)
 * SRegisters Registers;      // +8  (16 bytes: PB,DB,P,A,D,S,X,Y,PC)
 * uint8_t    _Carry;         // +24
 * uint8_t    _Zero;          // +25
 * uint8_t    _Negative;      // +26
 * uint8_t    _Overflow;      // +27
 */
.equ ICPU_OFF_CARRY, 24
.equ ICPU_OFF_ZERO, 25
.equ ICPU_OFF_NEGATIVE, 26
.equ ICPU_OFF_OVERFLOW, 27


/*
 * SetZN8_asm - Set Zero and Negative flags for 8-bit value
 * 
 * Equivalent to:
 *   ICPU._Zero = Work;
 *   ICPU._Negative = Work;
 *
 * Input:  r0 = 8-bit work value
 * Clobbers: r1
 */
.global SetZN8_asm  
.type SetZN8_asm, %function
.thumb_func
SetZN8_asm:
    ldr     r1, =ICPU
    strb    r0, [r1, #ICPU_OFF_ZERO]
    strb    r0, [r1, #ICPU_OFF_NEGATIVE]
    bx      lr

.size SetZN8_asm, . - SetZN8_asm


/*
 * SetZN16_asm - Set Zero and Negative flags for 16-bit value
 *
 * Equivalent to:
 *   ICPU._Zero = Work != 0;
 *   ICPU._Negative = (uint8_t)(Work >> 8);
 *
 * Input:  r0 = 16-bit work value
 * Clobbers: r1, r2
 */
.global SetZN16_asm
.type SetZN16_asm, %function
.thumb_func
SetZN16_asm:
    ldr     r1, =ICPU
    /* _Zero = (Work != 0) */
    cmp     r0, #0
    ite     ne
    movne   r2, #1
    moveq   r2, #0
    strb    r2, [r1, #ICPU_OFF_ZERO]
    /* _Negative = Work >> 8 */
    lsr     r2, r0, #8
    strb    r2, [r1, #ICPU_OFF_NEGATIVE]
    bx      lr

.size SetZN16_asm, . - SetZN16_asm

.end
