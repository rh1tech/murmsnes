/*
 * ARM Assembly optimized audio packing
 * Step 3: Assembly implementation of audio_pack_opt
 * 
 * Target: RP2350 Cortex-M33 @ 378-504 MHz
 */

    .syntax unified
    .cpu cortex-m33
    .thumb
    .section .time_critical.audio_pack,"ax",%progbits
    .align 2

/*
 * void audio_pack_asm(uint32_t* dst, const int16_t* src, uint32_t count,
 *                     int gain_num, int gain_den, bool use_soft_limit)
 * 
 * Args:
 *   r0 = dst (output uint32_t packed stereo)
 *   r1 = src (input int16_t stereo samples)
 *   r2 = count (number of stereo frames)
 *   r3 = gain_num
 *   [sp] = gain_den
 *   [sp+4] = use_soft_limit
 */
    .global audio_pack_asm
    .type audio_pack_asm, %function
    .thumb_func
audio_pack_asm:
    push    {r4-r11, lr}
    
    // Load stack parameters
    ldr     r4, [sp, #36]       // r4 = gain_den (9 regs * 4 bytes = 36)
    ldr     r5, [sp, #40]       // r5 = use_soft_limit
    
    // Early exit if count == 0
    cmp     r2, #0
    beq     .Lpack_done
    
    // Constants
    movw    r6, #32767          // max_pos
    movw    r7, #32768
    rsb     r7, r7, #0          // r7 = -32768
    movw    r8, #30000          // soft_limit knee_pos
    rsb     r9, r8, #0          // r9 = -30000
    
    // Check soft limit flag
    cmp     r5, #0
    bne     .Lloop_soft
    b       .Lloop_hard
    
    // === Soft limiter loop ===
.Lloop_soft:
    // Load left sample (sign-extended)
    ldrsh   r10, [r1], #2
    
    // Apply gain: left = (left * gain_num) / gain_den
    mul     r10, r10, r3
    sdiv    r10, r10, r4
    
    // Soft limit left
    cmp     r10, r8             // if (left > 30000)
    ble     .Lsoft_left_check_neg
    sub     r10, r10, r8        // left -= 30000
    asr     r10, r10, #2        // left /= 4
    add     r10, r10, r8        // left += 30000
    b       .Lclamp_left
    
.Lsoft_left_check_neg:
    cmp     r10, r9             // if (left < -30000)
    bge     .Lclamp_left
    sub     r10, r10, r9        // left -= (-30000) = left + 30000
    asr     r10, r10, #2        // left /= 4
    add     r10, r10, r9        // left += (-30000)
    
.Lclamp_left:
    // Clamp to [-32768, 32767]
    cmp     r10, r6
    it      gt
    movgt   r10, r6
    cmp     r10, r7
    it      lt
    movlt   r10, r7
    
    // Load right sample
    ldrsh   r11, [r1], #2
    mul     r11, r11, r3
    sdiv    r11, r11, r4
    
    // Soft limit right
    cmp     r11, r8
    ble     .Lsoft_right_check_neg
    sub     r11, r11, r8
    asr     r11, r11, #2
    add     r11, r11, r8
    b       .Lclamp_right
    
.Lsoft_right_check_neg:
    cmp     r11, r9
    bge     .Lclamp_right
    sub     r11, r11, r9
    asr     r11, r11, #2
    add     r11, r11, r9
    
.Lclamp_right:
    cmp     r11, r6
    it      gt
    movgt   r11, r6
    cmp     r11, r7
    it      lt
    movlt   r11, r7
    
    // Pack: dst[i] = (left << 16) | (right & 0xFFFF)
    uxth    r11, r11            // Zero-extend right to 16 bits
    lsl     r10, r10, #16       // left << 16
    orr     r10, r10, r11       // Combine
    str     r10, [r0], #4       // Store and advance
    
    subs    r2, r2, #1          // count--
    bne     .Lloop_soft
    b       .Lpack_done
    
    // === Hard clipping loop (no soft limiter) ===
.Lloop_hard:
    // Load left
    ldrsh   r10, [r1], #2
    mul     r10, r10, r3
    sdiv    r10, r10, r4
    
    // Clamp left
    cmp     r10, r6
    it      gt
    movgt   r10, r6
    cmp     r10, r7
    it      lt
    movlt   r10, r7
    
    // Load right
    ldrsh   r11, [r1], #2
    mul     r11, r11, r3
    sdiv    r11, r11, r4
    
    // Clamp right
    cmp     r11, r6
    it      gt
    movgt   r11, r6
    cmp     r11, r7
    it      lt
    movlt   r11, r7
    
    // Pack
    uxth    r11, r11
    lsl     r10, r10, #16
    orr     r10, r10, r11
    str     r10, [r0], #4
    
    subs    r2, r2, #1
    bne     .Lloop_hard
    
.Lpack_done:
    pop     {r4-r11, pc}
    
    .size audio_pack_asm, .-audio_pack_asm
