/*
 * HDMI Scanline Copy - ARM Thumb-2 Assembly (Cortex-M33)
 * 
 * Optimized scanline copy for palette-indexed HDMI output.
 * Copies pixels from 16-bit SNES screen buffer (low byte = palette index)
 * to 8-bit HDMI line buffer, substituting reserved HDMI sync indices.
 *
 * void hdmi_copy_scanline_asm(
 *     uint8_t* dst,           // r0: destination buffer (HDMI line)
 *     const uint16_t* src,    // r1: source buffer (SNES screen)
 *     uint32_t count,         // r2: pixel count (256)
 *     const uint8_t* subst    // r3: substitution table for indices 251-254
 * );
 */

    .syntax unified
    .cpu cortex-m33
    .thumb
    .section .scratch_y.hdmi_asm,"ax",%progbits

    .global hdmi_copy_scanline_asm
    .type hdmi_copy_scanline_asm, %function
    .align 4

/*
 * Register allocation:
 *   r0  = dst pointer (increments)
 *   r1  = src pointer (increments)
 *   r2  = remaining count (decrements)
 *   r3  = substitution table base
 *   r4  = scratch / pixel data
 *   r5  = scratch / packed output
 *   r6  = constant 251 (BASE_HDMI_CTRL_INX)
 *   r7  = constant 254 (max reserved index)
 *   r8-r11 = pixel scratch for unrolled loop
 *   r12 = temp
 */

hdmi_copy_scanline_asm:
    push    {r4-r7, lr}
    
    /* Load constants */
    movs    r6, #251            /* BASE_HDMI_CTRL_INX */
    movs    r7, #254            /* BASE_HDMI_CTRL_INX + 3 */
    
    /* Process 8 pixels at a time (main loop) */
    lsrs    r2, r2, #3          /* count /= 8 */
    beq     .L_remainder        /* if count < 8, skip main loop */

.L_loop8:
    /* Load 8 pixels (16 bytes from source = 8 x 16-bit values) */
    /* We only need the low byte of each 16-bit value */
    
    /* Pixel 0-3 */
    ldrh    r4, [r1, #0]        /* load pixel 0 (16-bit) */
    uxtb    r4, r4              /* extract low byte */
    cmp     r4, r6              /* compare with 251 */
    blo     .L_p0_ok
    cmp     r4, r7
    bhi     .L_p0_ok
    subs    r4, r4, r6          /* index = c - 251 */
    ldrb    r4, [r3, r4]        /* substitute */
.L_p0_ok:
    strb    r4, [r0, #0]
    
    ldrh    r4, [r1, #2]
    uxtb    r4, r4
    cmp     r4, r6
    blo     .L_p1_ok
    cmp     r4, r7
    bhi     .L_p1_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p1_ok:
    strb    r4, [r0, #1]
    
    ldrh    r4, [r1, #4]
    uxtb    r4, r4
    cmp     r4, r6
    blo     .L_p2_ok
    cmp     r4, r7
    bhi     .L_p2_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p2_ok:
    strb    r4, [r0, #2]
    
    ldrh    r4, [r1, #6]
    uxtb    r4, r4
    cmp     r4, r6
    blo     .L_p3_ok
    cmp     r4, r7
    bhi     .L_p3_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p3_ok:
    strb    r4, [r0, #3]
    
    /* Pixel 4-7 */
    ldrh    r4, [r1, #8]
    uxtb    r4, r4
    cmp     r4, r6
    blo     .L_p4_ok
    cmp     r4, r7
    bhi     .L_p4_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p4_ok:
    strb    r4, [r0, #4]
    
    ldrh    r4, [r1, #10]
    uxtb    r4, r4
    cmp     r4, r6
    blo     .L_p5_ok
    cmp     r4, r7
    bhi     .L_p5_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p5_ok:
    strb    r4, [r0, #5]
    
    ldrh    r4, [r1, #12]
    uxtb    r4, r4
    cmp     r4, r6
    blo     .L_p6_ok
    cmp     r4, r7
    bhi     .L_p6_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p6_ok:
    strb    r4, [r0, #6]
    
    ldrh    r4, [r1, #14]
    uxtb    r4, r4
    cmp     r4, r6
    blo     .L_p7_ok
    cmp     r4, r7
    bhi     .L_p7_ok
    subs    r4, r4, r6
    ldrb    r4, [r3, r4]
.L_p7_ok:
    strb    r4, [r0, #7]
    
    /* Advance pointers */
    adds    r0, r0, #8          /* dst += 8 */
    adds    r1, r1, #16         /* src += 8 (16-bit values) */
    
    subs    r2, r2, #1
    bne     .L_loop8

.L_remainder:
    /* No remainder since 256 is divisible by 8 */
    
    pop     {r4-r7, pc}
    
    .size hdmi_copy_scanline_asm, . - hdmi_copy_scanline_asm


/*
 * Fast memset for HDMI sync regions
 * 
 * void hdmi_memset_fast(uint8_t* dst, uint8_t val, uint32_t count);
 */
    .global hdmi_memset_fast
    .type hdmi_memset_fast, %function
    .align 4

hdmi_memset_fast:
    /* r0 = dst, r1 = val, r2 = count */
    push    {r4-r5, lr}
    
    /* Replicate byte to word */
    uxtb    r1, r1
    orr     r1, r1, r1, lsl #8   /* val | val<<8 */
    orr     r1, r1, r1, lsl #16  /* val | val<<8 | val<<16 | val<<24 */
    
    /* Store 4 bytes at a time */
    lsrs    r4, r2, #2           /* count / 4 */
    beq     .L_set_bytes
    
.L_set_words:
    str     r1, [r0], #4
    subs    r4, r4, #1
    bne     .L_set_words
    
.L_set_bytes:
    ands    r2, r2, #3           /* remaining bytes */
    beq     .L_set_done
    
.L_set_tail:
    strb    r1, [r0], #1
    subs    r2, r2, #1
    bne     .L_set_tail
    
.L_set_done:
    pop     {r4-r5, pc}
    
    .size hdmi_memset_fast, . - hdmi_memset_fast

    .end
