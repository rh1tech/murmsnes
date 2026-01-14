/* This file is part of Snes9x. See LICENSE file. */
/* Assembly-optimized tile rendering functions for ARM Cortex-M33 (RP2350) */

.syntax unified
.cpu cortex-m33
.thumb

/* Put in time-critical section for RAM execution */
.section .time_critical.snes9x_tile_asm, "ax", %progbits

/* External symbols */
.extern GFX
.extern BG
.extern Memory
.extern IPPU
.extern odd
.extern even

/* GFX struct offsets (from gfx.h):
 * The GFX struct contains rendering state.
 * Struct layout (all pointers are 4 bytes on ARM):
 *   uint8_t* Screen;       @ +0
 *   uint8_t* SubScreen;    @ +4
 *   uint8_t* ZBuffer;      @ +8
 *   uint8_t* SubZBuffer;   @ +12
 *   uint32_t Pitch;        @ +16
 *   int32_t  Delta;        @ +20
 *   uint16_t* ZERO;        @ +24
 *   uint32_t RealPitch;    @ +28
 *   uint32_t Pitch2;       @ +32
 *   uint32_t ZPitch;       @ +36
 *   uint32_t PPL;          @ +40
 *   uint32_t PPLx2;        @ +44
 *   uint32_t PixSize;      @ +48
 *   uint8_t* S;            @ +52  <- Screen buffer we write to
 *   uint8_t* DB;           @ +56  <- Depth buffer
 *   ptrdiff_t DepthDelta;  @ +60
 *   uint8_t  Z1;           @ +64  <- Depth compare value
 *   uint8_t  Z2;           @ +65  <- Depth write value
 */
.equ GFX_OFF_S, 52          @ Screen buffer (uint8_t* / uint16_t*)
.equ GFX_OFF_DB, 56         @ Depth buffer (uint8_t*)
.equ GFX_OFF_PPL, 40        @ Pixels per line
.equ GFX_OFF_Z1, 64         @ Z1 depth compare (uint8_t)
.equ GFX_OFF_Z2, 65         @ Z2 depth write (uint8_t)

/*
 * ConvertTile4bpp_asm - Convert a 4bpp SNES tile to 8bpp pixel cache
 *
 * This is the most common tile format (Mode 1 backgrounds).
 *
 * Input:
 *   r0 = pCache (uint8_t*) - destination cache buffer (64 bytes)
 *   r1 = TileAddr (uint32_t) - address in VRAM
 *
 * Output:
 *   r0 = tile flags (BLANK_TILE=2, or 0x10|depth with optional 0x20 opaque flag)
 *
 * C equivalent:
 *   for (line = 8; line != 0; line--, tp += 2) {
 *       p1 = odd[0][tp[0]>>4] | even[0][tp[1]>>4] | odd[1][tp[16]>>4] | even[1][tp[17]>>4];
 *       p2 = odd[0][tp[0]&0xf] | even[0][tp[1]&0xf] | odd[1][tp[16]&0xf] | even[1][tp[17]&0xf];
 *       *p++ = p1; *p++ = p2;
 *   }
 *
 * Array layout: odd[4][16] and even[4][16] are 2D arrays.
 * odd[i][j] is at address: odd + (i*16 + j) * 4
 * So odd[1][j] = odd + (16 + j) * 4 = odd + 64 + j*4
 */
.global ConvertTile4bpp_asm
.type ConvertTile4bpp_asm, %function
.thumb_func
ConvertTile4bpp_asm:
    push    {r4-r11, lr}
    sub     sp, sp, #12                 @ Local storage for odd1, even1 pointers
    
    /* r0 = pCache (output buffer), save it */
    str     r0, [sp, #0]                @ save pCache
    
    /* Get tp = &Memory.VRAM[TileAddr] */
    ldr     r2, =Memory
    ldr     r2, [r2]                    @ r2 = Memory.VRAM base
    add     r1, r2, r1                  @ r1 = tp = &Memory.VRAM[TileAddr]
    
    /* Load lookup table base addresses */
    ldr     r2, =odd                    @ r2 = &odd[0][0]
    ldr     r3, =even                   @ r3 = &even[0][0]
    
    /* Precompute odd[1] and even[1] base addresses */
    add     r4, r2, #64                 @ r4 = &odd[1][0]
    add     r5, r3, #64                 @ r5 = &even[1][0]
    str     r4, [sp, #4]                @ save odd1 base
    str     r5, [sp, #8]                @ save even1 base
    
    /* Accumulators */
    mov     r4, #0                      @ non_zero = 0
    mov     r5, #0                      @ has_transparent = 0
    
    /* Output pointer */
    ldr     r0, [sp, #0]                @ r0 = pCache
    
    /* Line counter */
    mov     r12, #8
    
.Lconv4_loop:
    /* Load tile bytes: tp[0], tp[1], tp[16], tp[17] */
    ldrb    r6, [r1, #0]                @ r6 = tp[0]
    ldrb    r7, [r1, #1]                @ r7 = tp[1]
    ldrb    r8, [r1, #16]               @ r8 = tp[16]
    ldrb    r9, [r1, #17]               @ r9 = tp[17]
    
    /* === Calculate p1 (high nibbles) === */
    
    /* p1 = odd[0][tp[0] >> 4] */
    lsr     r10, r6, #4
    ldr     r11, [r2, r10, lsl #2]      @ r11 = p1 = odd[0][...]
    
    /* p1 |= even[0][tp[1] >> 4] */
    lsr     r10, r7, #4
    ldr     r10, [r3, r10, lsl #2]
    orr     r11, r11, r10
    
    /* p1 |= odd[1][tp[16] >> 4] */
    ldr     r10, [sp, #4]               @ r10 = &odd[1][0]
    lsr     lr, r8, #4                  @ use lr as temp
    ldr     r10, [r10, lr, lsl #2]
    orr     r11, r11, r10
    
    /* p1 |= even[1][tp[17] >> 4] */
    ldr     r10, [sp, #8]               @ r10 = &even[1][0]
    lsr     lr, r9, #4
    ldr     r10, [r10, lr, lsl #2]
    orr     r11, r11, r10
    
    /* === Calculate p2 (low nibbles) === */
    
    /* p2 = odd[0][tp[0] & 0xf] */
    and     r10, r6, #0xf
    ldr     r10, [r2, r10, lsl #2]
    mov     r6, r10                     @ r6 = p2
    
    /* p2 |= even[0][tp[1] & 0xf] */
    and     r10, r7, #0xf
    ldr     r10, [r3, r10, lsl #2]
    orr     r6, r6, r10
    
    /* p2 |= odd[1][tp[16] & 0xf] */
    ldr     r10, [sp, #4]
    and     lr, r8, #0xf
    ldr     r10, [r10, lr, lsl #2]
    orr     r6, r6, r10
    
    /* p2 |= even[1][tp[17] & 0xf] */
    ldr     r10, [sp, #8]
    and     lr, r9, #0xf
    ldr     r10, [r10, lr, lsl #2]
    orr     r6, r6, r10
    
    /* Store p1, p2 to cache */
    str     r11, [r0], #4               @ *p++ = p1
    str     r6, [r0], #4                @ *p++ = p2
    
    /* non_zero |= p1 | p2 */
    orr     r7, r11, r6
    orr     r4, r4, r7
    
    /* has_transparent: check for zero bytes
     * HAS_ZERO_BYTE(x) = ((x - 0x01010101) & ~x & 0x80808080) != 0
     */
    ldr     r7, =0x01010101
    ldr     r8, =0x80808080
    
    /* Check p1 */
    sub     r9, r11, r7
    bic     r9, r9, r11
    and     r9, r9, r8
    orr     r5, r5, r9
    
    /* Check p2 */
    sub     r9, r6, r7
    bic     r9, r9, r6
    and     r9, r9, r8
    orr     r5, r5, r9
    
    /* Advance tp by 2 */
    add     r1, r1, #2
    
    /* Loop */
    subs    r12, r12, #1
    bne     .Lconv4_loop
    
    /* === Compute return value === */
    cmp     r4, #0
    beq     .Lconv4_blank
    
    /* Load BG.Depth - offset 36 in SBG struct */
    ldr     r0, =BG
    ldrb    r0, [r0, #36]
    orr     r0, r0, #0x10               @ 0x10 | BG.Depth
    
    cmp     r5, #0
    bne     .Lconv4_has_trans
    orr     r0, r0, #0x20               @ set opaque flag
.Lconv4_has_trans:
    add     sp, sp, #12
    pop     {r4-r11, pc}
    
.Lconv4_blank:
    mov     r0, #2                      @ BLANK_TILE = 2
    add     sp, sp, #12
    pop     {r4-r11, pc}

.size ConvertTile4bpp_asm, .-ConvertTile4bpp_asm


/*
 * WRITE_4PIXELS16_OPAQUE_asm - Write 4 opaque pixels with depth test only
 *
 * This is the inner loop for opaque tile rendering. For each pixel:
 *   if (Z1 > Depth[i]) { Screen[i] = ScreenColors[Pixels[i]]; Depth[i] = Z2; }
 *
 * Input:
 *   r0 = Offset (int32_t) - offset into screen/depth buffers
 *   r1 = Pixels (uint8_t*) - pointer to 4 pixel indices
 *   r2 = ScreenColors (uint16_t*) - palette lookup table
 *
 * Clobbers: r0-r3, r12
 */
.global WRITE_4PIXELS16_OPAQUE_asm
.type WRITE_4PIXELS16_OPAQUE_asm, %function
.thumb_func
WRITE_4PIXELS16_OPAQUE_asm:
    push    {r4-r7, lr}
    
    /* Load GFX struct fields */
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]        @ r4 = GFX.S (Screen base)
    ldr     r5, [r3, #GFX_OFF_DB]       @ r5 = GFX.DB (Depth base)
    ldrb    r6, [r3, #GFX_OFF_Z1]       @ r6 = Z1 (depth test)
    ldrb    r7, [r3, #GFX_OFF_Z2]       @ r7 = Z2 (depth write)
    
    /* Calculate buffer addresses: Screen = GFX.S + Offset*2, Depth = GFX.DB + Offset */
    add     r5, r5, r0                  @ r5 = Depth = GFX.DB + Offset
    add     r4, r4, r0, lsl #1          @ r4 = Screen = GFX.S + Offset*2
    
    /* Load all 4 pixel indices at once */
    ldr     r3, [r1]                    @ r3 = Pixels[0-3] packed
    
    /* Pixel 0 */
    ldrb    r12, [r5, #0]               @ r12 = Depth[0]
    cmp     r6, r12                     @ Z1 > Depth[0]?
    bls     .Lp0_skip
    and     r0, r3, #0xFF               @ r0 = Pixels[0]
    ldrh    r0, [r2, r0, lsl #1]        @ r0 = ScreenColors[Pixels[0]]
    strh    r0, [r4, #0]                @ Screen[0] = color
    strb    r7, [r5, #0]                @ Depth[0] = Z2
.Lp0_skip:

    /* Pixel 1 */
    ldrb    r12, [r5, #1]               @ r12 = Depth[1]
    cmp     r6, r12                     @ Z1 > Depth[1]?
    bls     .Lp1_skip
    ubfx    r0, r3, #8, #8              @ r0 = Pixels[1]
    ldrh    r0, [r2, r0, lsl #1]        @ r0 = ScreenColors[Pixels[1]]
    strh    r0, [r4, #2]                @ Screen[1] = color
    strb    r7, [r5, #1]                @ Depth[1] = Z2
.Lp1_skip:

    /* Pixel 2 */
    ldrb    r12, [r5, #2]               @ r12 = Depth[2]
    cmp     r6, r12                     @ Z1 > Depth[2]?
    bls     .Lp2_skip
    ubfx    r0, r3, #16, #8             @ r0 = Pixels[2]
    ldrh    r0, [r2, r0, lsl #1]        @ r0 = ScreenColors[Pixels[2]]
    strh    r0, [r4, #4]                @ Screen[2] = color
    strb    r7, [r5, #2]                @ Depth[2] = Z2
.Lp2_skip:

    /* Pixel 3 */
    ldrb    r12, [r5, #3]               @ r12 = Depth[3]
    cmp     r6, r12                     @ Z1 > Depth[3]?
    bls     .Lp3_skip
    lsr     r0, r3, #24                 @ r0 = Pixels[3]
    ldrh    r0, [r2, r0, lsl #1]        @ r0 = ScreenColors[Pixels[3]]
    strh    r0, [r4, #6]                @ Screen[3] = color
    strb    r7, [r5, #3]                @ Depth[3] = Z2
.Lp3_skip:

    pop     {r4-r7, pc}
.size WRITE_4PIXELS16_OPAQUE_asm, .-WRITE_4PIXELS16_OPAQUE_asm


/*
 * WRITE_4PIXELS16_FLIPPED_OPAQUE_asm - Write 4 opaque pixels (H-flipped) with depth test
 *
 * Same as above but reads pixels in reverse order (for horizontal flip)
 *
 * Input:
 *   r0 = Offset (int32_t)
 *   r1 = Pixels (uint8_t*)
 *   r2 = ScreenColors (uint16_t*)
 */
.global WRITE_4PIXELS16_FLIPPED_OPAQUE_asm
.type WRITE_4PIXELS16_FLIPPED_OPAQUE_asm, %function
.thumb_func
WRITE_4PIXELS16_FLIPPED_OPAQUE_asm:
    push    {r4-r7, lr}
    
    /* Load GFX struct fields */
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]        @ r4 = GFX.S (Screen base)
    ldr     r5, [r3, #GFX_OFF_DB]       @ r5 = GFX.DB (Depth base)
    ldrb    r6, [r3, #GFX_OFF_Z1]       @ r6 = Z1 (depth test)
    ldrb    r7, [r3, #GFX_OFF_Z2]       @ r7 = Z2 (depth write)
    
    /* Calculate buffer addresses */
    add     r5, r5, r0                  @ r5 = Depth = GFX.DB + Offset
    add     r4, r4, r0, lsl #1          @ r4 = Screen = GFX.S + Offset*2
    
    /* Load all 4 pixel indices at once */
    ldr     r3, [r1]                    @ r3 = Pixels[0-3] packed
    
    /* Pixel 0 <- Pixels[3] (flipped) */
    ldrb    r12, [r5, #0]               @ r12 = Depth[0]
    cmp     r6, r12
    bls     .Lf0_skip
    lsr     r0, r3, #24                 @ r0 = Pixels[3]
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #0]
    strb    r7, [r5, #0]
.Lf0_skip:

    /* Pixel 1 <- Pixels[2] */
    ldrb    r12, [r5, #1]
    cmp     r6, r12
    bls     .Lf1_skip
    ubfx    r0, r3, #16, #8             @ r0 = Pixels[2]
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #2]
    strb    r7, [r5, #1]
.Lf1_skip:

    /* Pixel 2 <- Pixels[1] */
    ldrb    r12, [r5, #2]
    cmp     r6, r12
    bls     .Lf2_skip
    ubfx    r0, r3, #8, #8              @ r0 = Pixels[1]
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strb    r7, [r5, #2]
.Lf2_skip:

    /* Pixel 3 <- Pixels[0] */
    ldrb    r12, [r5, #3]
    cmp     r6, r12
    bls     .Lf3_skip
    and     r0, r3, #0xFF               @ r0 = Pixels[0]
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #6]
    strb    r7, [r5, #3]
.Lf3_skip:

    pop     {r4-r7, pc}
.size WRITE_4PIXELS16_FLIPPED_OPAQUE_asm, .-WRITE_4PIXELS16_FLIPPED_OPAQUE_asm


/*
 * WRITE_4PIXELS16_asm - Write 4 pixels with transparency and depth test
 *
 * For each pixel: if (Z1 > Depth[i] && Pixels[i] != 0) { write pixel }
 * This is the standard path for tiles with some transparent pixels.
 *
 * Input:
 *   r0 = Offset (int32_t)
 *   r1 = Pixels (uint8_t*)
 *   r2 = ScreenColors (uint16_t*)
 */
.global WRITE_4PIXELS16_asm
.type WRITE_4PIXELS16_asm, %function
.thumb_func
WRITE_4PIXELS16_asm:
    push    {r4-r7, lr}
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]        @ r4 = GFX.S
    ldr     r5, [r3, #GFX_OFF_DB]       @ r5 = GFX.DB
    ldrb    r6, [r3, #GFX_OFF_Z1]       @ r6 = Z1
    ldrb    r7, [r3, #GFX_OFF_Z2]       @ r7 = Z2
    
    add     r5, r5, r0                  @ r5 = Depth
    add     r4, r4, r0, lsl #1          @ r4 = Screen
    
    ldr     r3, [r1]                    @ r3 = Pixels[0-3] packed
    
    /* Pixel 0 */
    and     r0, r3, #0xFF               @ r0 = Pixels[0]
    cbz     r0, .Lt0_skip               @ skip if transparent
    ldrb    r12, [r5, #0]
    cmp     r6, r12
    bls     .Lt0_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #0]
    strb    r7, [r5, #0]
.Lt0_skip:

    /* Pixel 1 */
    ubfx    r0, r3, #8, #8              @ r0 = Pixels[1]
    cbz     r0, .Lt1_skip
    ldrb    r12, [r5, #1]
    cmp     r6, r12
    bls     .Lt1_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #2]
    strb    r7, [r5, #1]
.Lt1_skip:

    /* Pixel 2 */
    ubfx    r0, r3, #16, #8             @ r0 = Pixels[2]
    cbz     r0, .Lt2_skip
    ldrb    r12, [r5, #2]
    cmp     r6, r12
    bls     .Lt2_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strb    r7, [r5, #2]
.Lt2_skip:

    /* Pixel 3 */
    lsr     r0, r3, #24                 @ r0 = Pixels[3]
    cbz     r0, .Lt3_skip
    ldrb    r12, [r5, #3]
    cmp     r6, r12
    bls     .Lt3_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #6]
    strb    r7, [r5, #3]
.Lt3_skip:

    pop     {r4-r7, pc}
.size WRITE_4PIXELS16_asm, .-WRITE_4PIXELS16_asm


/*
 * WRITE_4PIXELS16_FLIPPED_asm - Write 4 pixels (H-flipped) with transparency
 */
.global WRITE_4PIXELS16_FLIPPED_asm
.type WRITE_4PIXELS16_FLIPPED_asm, %function
.thumb_func
WRITE_4PIXELS16_FLIPPED_asm:
    push    {r4-r7, lr}
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r0
    add     r4, r4, r0, lsl #1
    
    ldr     r3, [r1]                    @ r3 = Pixels[0-3] packed
    
    /* Pixel 0 <- Pixels[3] */
    lsr     r0, r3, #24
    cbz     r0, .Ltf0_skip
    ldrb    r12, [r5, #0]
    cmp     r6, r12
    bls     .Ltf0_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #0]
    strb    r7, [r5, #0]
.Ltf0_skip:

    /* Pixel 1 <- Pixels[2] */
    ubfx    r0, r3, #16, #8
    cbz     r0, .Ltf1_skip
    ldrb    r12, [r5, #1]
    cmp     r6, r12
    bls     .Ltf1_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #2]
    strb    r7, [r5, #1]
.Ltf1_skip:

    /* Pixel 2 <- Pixels[1] */
    ubfx    r0, r3, #8, #8
    cbz     r0, .Ltf2_skip
    ldrb    r12, [r5, #2]
    cmp     r6, r12
    bls     .Ltf2_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strb    r7, [r5, #2]
.Ltf2_skip:

    /* Pixel 3 <- Pixels[0] */
    and     r0, r3, #0xFF
    cbz     r0, .Ltf3_skip
    ldrb    r12, [r5, #3]
    cmp     r6, r12
    bls     .Ltf3_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #6]
    strb    r7, [r5, #3]
.Ltf3_skip:

    pop     {r4-r7, pc}
.size WRITE_4PIXELS16_FLIPPED_asm, .-WRITE_4PIXELS16_FLIPPED_asm


/*
 * WRITE_4PIXELS16x2_OPAQUE_asm - Write 4 doubled pixels (opaque)
 *
 * Each pixel is written twice (Screen[N*2] and Screen[N*2+1])
 * Used for x2 width tile modes.
 */
.global WRITE_4PIXELS16x2_OPAQUE_asm
.type WRITE_4PIXELS16x2_OPAQUE_asm, %function
.thumb_func
WRITE_4PIXELS16x2_OPAQUE_asm:
    push    {r4-r7, lr}
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r0                  @ r5 = Depth
    add     r4, r4, r0, lsl #1          @ r4 = Screen
    
    ldr     r3, [r1]                    @ r3 = Pixels[0-3] packed
    
    /* Pixel 0: Screen[0,1] = color, Depth[0,1] = Z2 */
    ldrb    r12, [r5, #0]
    cmp     r6, r12
    bls     .Lx0_skip
    and     r0, r3, #0xFF
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #0]
    strh    r0, [r4, #2]
    strb    r7, [r5, #0]
    strb    r7, [r5, #1]
.Lx0_skip:

    /* Pixel 1: Screen[2,3] */
    ldrb    r12, [r5, #2]
    cmp     r6, r12
    bls     .Lx1_skip
    ubfx    r0, r3, #8, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strh    r0, [r4, #6]
    strb    r7, [r5, #2]
    strb    r7, [r5, #3]
.Lx1_skip:

    /* Pixel 2: Screen[4,5] */
    ldrb    r12, [r5, #4]
    cmp     r6, r12
    bls     .Lx2_skip
    ubfx    r0, r3, #16, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #8]
    strh    r0, [r4, #10]
    strb    r7, [r5, #4]
    strb    r7, [r5, #5]
.Lx2_skip:

    /* Pixel 3: Screen[6,7] */
    ldrb    r12, [r5, #6]
    cmp     r6, r12
    bls     .Lx3_skip
    lsr     r0, r3, #24
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #12]
    strh    r0, [r4, #14]
    strb    r7, [r5, #6]
    strb    r7, [r5, #7]
.Lx3_skip:

    pop     {r4-r7, pc}
.size WRITE_4PIXELS16x2_OPAQUE_asm, .-WRITE_4PIXELS16x2_OPAQUE_asm


/*
 * WRITE_4PIXELS16_FLIPPEDx2_OPAQUE_asm - Write 4 doubled pixels (H-flipped, opaque)
 */
.global WRITE_4PIXELS16_FLIPPEDx2_OPAQUE_asm
.type WRITE_4PIXELS16_FLIPPEDx2_OPAQUE_asm, %function
.thumb_func
WRITE_4PIXELS16_FLIPPEDx2_OPAQUE_asm:
    push    {r4-r7, lr}
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r0
    add     r4, r4, r0, lsl #1
    
    ldr     r3, [r1]
    
    /* Pixel 0 <- Pixels[3] */
    ldrb    r12, [r5, #0]
    cmp     r6, r12
    bls     .Lxf0_skip
    lsr     r0, r3, #24
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #0]
    strh    r0, [r4, #2]
    strb    r7, [r5, #0]
    strb    r7, [r5, #1]
.Lxf0_skip:

    /* Pixel 1 <- Pixels[2] */
    ldrb    r12, [r5, #2]
    cmp     r6, r12
    bls     .Lxf1_skip
    ubfx    r0, r3, #16, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strh    r0, [r4, #6]
    strb    r7, [r5, #2]
    strb    r7, [r5, #3]
.Lxf1_skip:

    /* Pixel 2 <- Pixels[1] */
    ldrb    r12, [r5, #4]
    cmp     r6, r12
    bls     .Lxf2_skip
    ubfx    r0, r3, #8, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #8]
    strh    r0, [r4, #10]
    strb    r7, [r5, #4]
    strb    r7, [r5, #5]
.Lxf2_skip:

    /* Pixel 3 <- Pixels[0] */
    ldrb    r12, [r5, #6]
    cmp     r6, r12
    bls     .Lxf3_skip
    and     r0, r3, #0xFF
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #12]
    strh    r0, [r4, #14]
    strb    r7, [r5, #6]
    strb    r7, [r5, #7]
.Lxf3_skip:

    pop     {r4-r7, pc}
.size WRITE_4PIXELS16_FLIPPEDx2_OPAQUE_asm, .-WRITE_4PIXELS16_FLIPPEDx2_OPAQUE_asm


/*
 * WRITE_4PIXELS16x2_asm - Write 4 doubled pixels with transparency
 *
 * Each pixel is written twice (Screen[N*2] and Screen[N*2+1])
 * Includes transparency check (skip if pixel == 0)
 */
.global WRITE_4PIXELS16x2_asm
.type WRITE_4PIXELS16x2_asm, %function
.thumb_func
WRITE_4PIXELS16x2_asm:
    push    {r4-r7, lr}
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r0                  @ r5 = Depth
    add     r4, r4, r0, lsl #1          @ r4 = Screen
    
    ldr     r3, [r1]                    @ r3 = Pixels[0-3] packed
    
    /* Pixel 0: Screen[0,1] */
    and     r0, r3, #0xFF               @ r0 = Pixels[0]
    cbz     r0, .Lx2t0_skip             @ skip if transparent
    ldrb    r12, [r5, #0]
    cmp     r6, r12
    bls     .Lx2t0_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #0]
    strh    r0, [r4, #2]
    strb    r7, [r5, #0]
    strb    r7, [r5, #1]
.Lx2t0_skip:

    /* Pixel 1: Screen[2,3] */
    ubfx    r0, r3, #8, #8              @ r0 = Pixels[1]
    cbz     r0, .Lx2t1_skip
    ldrb    r12, [r5, #2]
    cmp     r6, r12
    bls     .Lx2t1_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strh    r0, [r4, #6]
    strb    r7, [r5, #2]
    strb    r7, [r5, #3]
.Lx2t1_skip:

    /* Pixel 2: Screen[4,5] */
    ubfx    r0, r3, #16, #8             @ r0 = Pixels[2]
    cbz     r0, .Lx2t2_skip
    ldrb    r12, [r5, #4]
    cmp     r6, r12
    bls     .Lx2t2_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #8]
    strh    r0, [r4, #10]
    strb    r7, [r5, #4]
    strb    r7, [r5, #5]
.Lx2t2_skip:

    /* Pixel 3: Screen[6,7] */
    lsr     r0, r3, #24                 @ r0 = Pixels[3]
    cbz     r0, .Lx2t3_skip
    ldrb    r12, [r5, #6]
    cmp     r6, r12
    bls     .Lx2t3_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #12]
    strh    r0, [r4, #14]
    strb    r7, [r5, #6]
    strb    r7, [r5, #7]
.Lx2t3_skip:

    pop     {r4-r7, pc}
.size WRITE_4PIXELS16x2_asm, .-WRITE_4PIXELS16x2_asm


/*
 * WRITE_4PIXELS16_FLIPPEDx2_asm - Write 4 doubled pixels (H-flipped) with transparency
 */
.global WRITE_4PIXELS16_FLIPPEDx2_asm
.type WRITE_4PIXELS16_FLIPPEDx2_asm, %function
.thumb_func
WRITE_4PIXELS16_FLIPPEDx2_asm:
    push    {r4-r7, lr}
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r0
    add     r4, r4, r0, lsl #1
    
    ldr     r3, [r1]                    @ r3 = Pixels[0-3] packed
    
    /* Pixel 0 <- Pixels[3] */
    lsr     r0, r3, #24
    cbz     r0, .Lx2tf0_skip
    ldrb    r12, [r5, #0]
    cmp     r6, r12
    bls     .Lx2tf0_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #0]
    strh    r0, [r4, #2]
    strb    r7, [r5, #0]
    strb    r7, [r5, #1]
.Lx2tf0_skip:

    /* Pixel 1 <- Pixels[2] */
    ubfx    r0, r3, #16, #8
    cbz     r0, .Lx2tf1_skip
    ldrb    r12, [r5, #2]
    cmp     r6, r12
    bls     .Lx2tf1_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strh    r0, [r4, #6]
    strb    r7, [r5, #2]
    strb    r7, [r5, #3]
.Lx2tf1_skip:

    /* Pixel 2 <- Pixels[1] */
    ubfx    r0, r3, #8, #8
    cbz     r0, .Lx2tf2_skip
    ldrb    r12, [r5, #4]
    cmp     r6, r12
    bls     .Lx2tf2_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #8]
    strh    r0, [r4, #10]
    strb    r7, [r5, #4]
    strb    r7, [r5, #5]
.Lx2tf2_skip:

    /* Pixel 3 <- Pixels[0] */
    and     r0, r3, #0xFF
    cbz     r0, .Lx2tf3_skip
    ldrb    r12, [r5, #6]
    cmp     r6, r12
    bls     .Lx2tf3_skip
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #12]
    strh    r0, [r4, #14]
    strb    r7, [r5, #6]
    strb    r7, [r5, #7]
.Lx2tf3_skip:

    pop     {r4-r7, pc}
.size WRITE_4PIXELS16_FLIPPEDx2_asm, .-WRITE_4PIXELS16_FLIPPEDx2_asm


/*
 * WRITE_8PIXELS16_OPAQUE_ROW_asm - Write a full 8-pixel tile row (opaque)
 *
 * This combines both halves of a tile row into one call to reduce overhead.
 * Saves: 1 push/pop pair, 1 GFX load, 1 function call/return per row.
 *
 * Input:
 *   r0 = Offset (int32_t) - offset into screen/depth buffers
 *   r1 = Pixels (uint8_t*) - pointer to 8 pixel indices (full row)
 *   r2 = ScreenColors (uint16_t*) - palette lookup table
 *
 * Clobbers: r0-r3, r12
 */
.global WRITE_8PIXELS16_OPAQUE_ROW_asm
.type WRITE_8PIXELS16_OPAQUE_ROW_asm, %function
.thumb_func
WRITE_8PIXELS16_OPAQUE_ROW_asm:
    push    {r4-r11, lr}
    
    /* Load GFX struct fields once for all 8 pixels */
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]        @ r4 = GFX.S (Screen base)
    ldr     r5, [r3, #GFX_OFF_DB]       @ r5 = GFX.DB (Depth base)
    ldrb    r6, [r3, #GFX_OFF_Z1]       @ r6 = Z1 (depth test)
    ldrb    r7, [r3, #GFX_OFF_Z2]       @ r7 = Z2 (depth write)
    
    /* Calculate buffer addresses */
    add     r5, r5, r0                  @ r5 = Depth = GFX.DB + Offset
    add     r4, r4, r0, lsl #1          @ r4 = Screen = GFX.S + Offset*2
    
    /* Load all 8 pixel indices (two 32-bit loads) */
    ldm     r1, {r8, r9}                @ r8 = Pixels[0-3], r9 = Pixels[4-7]
    
    /* === First 4 pixels (r8) === */
    
    /* Pixel 0 */
    ldrb    r10, [r5, #0]               @ r10 = Depth[0]
    cmp     r6, r10                     @ Z1 > Depth[0]?
    bls     .L8o_p0_skip
    and     r0, r8, #0xFF               @ r0 = Pixels[0]
    ldrh    r0, [r2, r0, lsl #1]        @ r0 = ScreenColors[Pixels[0]]
    strh    r0, [r4, #0]                @ Screen[0] = color
    strb    r7, [r5, #0]                @ Depth[0] = Z2
.L8o_p0_skip:

    /* Pixel 1 */
    ldrb    r10, [r5, #1]
    cmp     r6, r10
    bls     .L8o_p1_skip
    ubfx    r0, r8, #8, #8              @ r0 = Pixels[1]
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #2]
    strb    r7, [r5, #1]
.L8o_p1_skip:

    /* Pixel 2 */
    ldrb    r10, [r5, #2]
    cmp     r6, r10
    bls     .L8o_p2_skip
    ubfx    r0, r8, #16, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strb    r7, [r5, #2]
.L8o_p2_skip:

    /* Pixel 3 */
    ldrb    r10, [r5, #3]
    cmp     r6, r10
    bls     .L8o_p3_skip
    lsr     r0, r8, #24
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #6]
    strb    r7, [r5, #3]
.L8o_p3_skip:

    /* === Second 4 pixels (r9) === */
    
    /* Pixel 4 */
    ldrb    r10, [r5, #4]
    cmp     r6, r10
    bls     .L8o_p4_skip
    and     r0, r9, #0xFF
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #8]
    strb    r7, [r5, #4]
.L8o_p4_skip:

    /* Pixel 5 */
    ldrb    r10, [r5, #5]
    cmp     r6, r10
    bls     .L8o_p5_skip
    ubfx    r0, r9, #8, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #10]
    strb    r7, [r5, #5]
.L8o_p5_skip:

    /* Pixel 6 */
    ldrb    r10, [r5, #6]
    cmp     r6, r10
    bls     .L8o_p6_skip
    ubfx    r0, r9, #16, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #12]
    strb    r7, [r5, #6]
.L8o_p6_skip:

    /* Pixel 7 */
    ldrb    r10, [r5, #7]
    cmp     r6, r10
    bls     .L8o_p7_skip
    lsr     r0, r9, #24
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #14]
    strb    r7, [r5, #7]
.L8o_p7_skip:

    pop     {r4-r11, pc}
.size WRITE_8PIXELS16_OPAQUE_ROW_asm, .-WRITE_8PIXELS16_OPAQUE_ROW_asm


/*
 * WRITE_8PIXELS16_FLIPPED_OPAQUE_ROW_asm - Write a full 8-pixel tile row (opaque, H-flipped)
 *
 * Same as above but reads pixels in reverse order.
 */
.global WRITE_8PIXELS16_FLIPPED_OPAQUE_ROW_asm
.type WRITE_8PIXELS16_FLIPPED_OPAQUE_ROW_asm, %function
.thumb_func
WRITE_8PIXELS16_FLIPPED_OPAQUE_ROW_asm:
    push    {r4-r11, lr}
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r0
    add     r4, r4, r0, lsl #1
    
    ldm     r1, {r8, r9}                @ r8 = Pixels[0-3], r9 = Pixels[4-7]
    
    /* Flipped: Screen[0] <- Pixels[7], Screen[1] <- Pixels[6], etc */
    
    /* Pixel 0 <- Pixels[7] (from r9 byte 3) */
    ldrb    r10, [r5, #0]
    cmp     r6, r10
    bls     .L8of_p0_skip
    lsr     r0, r9, #24
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #0]
    strb    r7, [r5, #0]
.L8of_p0_skip:

    /* Pixel 1 <- Pixels[6] (from r9 byte 2) */
    ldrb    r10, [r5, #1]
    cmp     r6, r10
    bls     .L8of_p1_skip
    ubfx    r0, r9, #16, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #2]
    strb    r7, [r5, #1]
.L8of_p1_skip:

    /* Pixel 2 <- Pixels[5] (from r9 byte 1) */
    ldrb    r10, [r5, #2]
    cmp     r6, r10
    bls     .L8of_p2_skip
    ubfx    r0, r9, #8, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #4]
    strb    r7, [r5, #2]
.L8of_p2_skip:

    /* Pixel 3 <- Pixels[4] (from r9 byte 0) */
    ldrb    r10, [r5, #3]
    cmp     r6, r10
    bls     .L8of_p3_skip
    and     r0, r9, #0xFF
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #6]
    strb    r7, [r5, #3]
.L8of_p3_skip:

    /* Pixel 4 <- Pixels[3] (from r8 byte 3) */
    ldrb    r10, [r5, #4]
    cmp     r6, r10
    bls     .L8of_p4_skip
    lsr     r0, r8, #24
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #8]
    strb    r7, [r5, #4]
.L8of_p4_skip:

    /* Pixel 5 <- Pixels[2] (from r8 byte 2) */
    ldrb    r10, [r5, #5]
    cmp     r6, r10
    bls     .L8of_p5_skip
    ubfx    r0, r8, #16, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #10]
    strb    r7, [r5, #5]
.L8of_p5_skip:

    /* Pixel 6 <- Pixels[1] (from r8 byte 1) */
    ldrb    r10, [r5, #6]
    cmp     r6, r10
    bls     .L8of_p6_skip
    ubfx    r0, r8, #8, #8
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #12]
    strb    r7, [r5, #6]
.L8of_p6_skip:

    /* Pixel 7 <- Pixels[0] (from r8 byte 0) */
    ldrb    r10, [r5, #7]
    cmp     r6, r10
    bls     .L8of_p7_skip
    and     r0, r8, #0xFF
    ldrh    r0, [r2, r0, lsl #1]
    strh    r0, [r4, #14]
    strb    r7, [r5, #7]
.L8of_p7_skip:

    pop     {r4-r11, pc}
.size WRITE_8PIXELS16_FLIPPED_OPAQUE_ROW_asm, .-WRITE_8PIXELS16_FLIPPED_OPAQUE_ROW_asm


/*
 * DrawTile16_Normal_asm - Draw a complete 8x8 tile (no flip) with depth test
 *
 * This is a fully integrated tile renderer that handles all 8 rows
 * in a single function call, eliminating per-row function call overhead.
 *
 * Input:
 *   r0 = pCache (uint8_t*) - pointer to 64-byte cached tile pixels
 *   r1 = Offset (int32_t) - starting screen buffer offset
 *   r2 = ScreenColors (uint16_t*) - palette lookup table
 *   r3 = StartLine (uint32_t) - starting line within tile (0-56, step 8)
 *   [sp] = LineCount (uint32_t) - number of lines to draw (1-8)
 *
 * Optimizations:
 *   - Single GFX struct load for all rows
 *   - Pipelined pixel processing
 *   - Reduced branch overhead
 */
.global DrawTile16_Normal_asm
.type DrawTile16_Normal_asm, %function
.thumb_func
DrawTile16_Normal_asm:
    push    {r4-r11, lr}
    
    /* Load LineCount from stack */
    ldr     r12, [sp, #36]              @ LineCount (after 9 regs pushed)
    
    /* Setup: bp = pCache + StartLine */
    add     r0, r0, r3                  @ r0 = bp = pCache + StartLine
    
    /* Load GFX struct fields once */
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]        @ r4 = GFX.S base
    ldr     r5, [r3, #GFX_OFF_DB]       @ r5 = GFX.DB base
    ldr     r11, [r3, #GFX_OFF_PPL]     @ r11 = GFX.PPL
    ldrb    r6, [r3, #GFX_OFF_Z1]       @ r6 = Z1
    ldrb    r7, [r3, #GFX_OFF_Z2]       @ r7 = Z2
    
    /* Calculate initial screen/depth pointers */
    add     r5, r5, r1                  @ r5 = Depth = GFX.DB + Offset
    add     r4, r4, r1, lsl #1          @ r4 = Screen = GFX.S + Offset*2
    
.Ldraw_normal_loop:
    /* Load 8 pixels (two 32-bit loads) */
    ldm     r0, {r8, r9}                @ r8 = pixels[0-3], r9 = pixels[4-7]
    
    /* === Pixels 0-3 (from r8) === */
    
    /* Pixel 0 */
    ldrb    r10, [r5, #0]
    cmp     r6, r10
    bls     .Ldn_p0_skip
    and     r3, r8, #0xFF
    cbz     r3, .Ldn_p0_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #0]
    strb    r7, [r5, #0]
.Ldn_p0_skip:

    /* Pixel 1 */
    ldrb    r10, [r5, #1]
    cmp     r6, r10
    bls     .Ldn_p1_skip
    ubfx    r3, r8, #8, #8
    cbz     r3, .Ldn_p1_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #2]
    strb    r7, [r5, #1]
.Ldn_p1_skip:

    /* Pixel 2 */
    ldrb    r10, [r5, #2]
    cmp     r6, r10
    bls     .Ldn_p2_skip
    ubfx    r3, r8, #16, #8
    cbz     r3, .Ldn_p2_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #4]
    strb    r7, [r5, #2]
.Ldn_p2_skip:

    /* Pixel 3 */
    ldrb    r10, [r5, #3]
    cmp     r6, r10
    bls     .Ldn_p3_skip
    lsr     r3, r8, #24
    cbz     r3, .Ldn_p3_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #6]
    strb    r7, [r5, #3]
.Ldn_p3_skip:

    /* === Pixels 4-7 (from r9) === */
    
    /* Pixel 4 */
    ldrb    r10, [r5, #4]
    cmp     r6, r10
    bls     .Ldn_p4_skip
    and     r3, r9, #0xFF
    cbz     r3, .Ldn_p4_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #8]
    strb    r7, [r5, #4]
.Ldn_p4_skip:

    /* Pixel 5 */
    ldrb    r10, [r5, #5]
    cmp     r6, r10
    bls     .Ldn_p5_skip
    ubfx    r3, r9, #8, #8
    cbz     r3, .Ldn_p5_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #10]
    strb    r7, [r5, #5]
.Ldn_p5_skip:

    /* Pixel 6 */
    ldrb    r10, [r5, #6]
    cmp     r6, r10
    bls     .Ldn_p6_skip
    ubfx    r3, r9, #16, #8
    cbz     r3, .Ldn_p6_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #12]
    strb    r7, [r5, #6]
.Ldn_p6_skip:

    /* Pixel 7 */
    ldrb    r10, [r5, #7]
    cmp     r6, r10
    bls     .Ldn_p7_skip
    lsr     r3, r9, #24
    cbz     r3, .Ldn_p7_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #14]
    strb    r7, [r5, #7]
.Ldn_p7_skip:

    /* Advance to next row */
    add     r0, r0, #8                  @ bp += 8
    add     r4, r4, r11, lsl #1         @ Screen += PPL * 2 (16-bit pixels)
    add     r5, r5, r11                 @ Depth += PPL
    
    subs    r12, r12, #1                @ LineCount--
    bne     .Ldraw_normal_loop
    
    pop     {r4-r11, pc}
.size DrawTile16_Normal_asm, .-DrawTile16_Normal_asm


/*
 * DrawTile16_FlipH_asm - Draw a complete 8x8 tile (H-flipped) with depth test
 */
.global DrawTile16_FlipH_asm
.type DrawTile16_FlipH_asm, %function
.thumb_func
DrawTile16_FlipH_asm:
    push    {r4-r11, lr}
    
    ldr     r12, [sp, #36]              @ LineCount
    add     r0, r0, r3                  @ bp = pCache + StartLine
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldr     r11, [r3, #GFX_OFF_PPL]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r1
    add     r4, r4, r1, lsl #1
    
.Ldraw_fliph_loop:
    ldm     r0, {r8, r9}                @ r8 = pixels[0-3], r9 = pixels[4-7]
    
    /* Flipped: Screen[0] <- pixels[7], Screen[7] <- pixels[0] */
    
    /* Pixel 0 <- pixels[7] */
    ldrb    r10, [r5, #0]
    cmp     r6, r10
    bls     .Ldh_p0_skip
    lsr     r3, r9, #24
    cbz     r3, .Ldh_p0_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #0]
    strb    r7, [r5, #0]
.Ldh_p0_skip:

    /* Pixel 1 <- pixels[6] */
    ldrb    r10, [r5, #1]
    cmp     r6, r10
    bls     .Ldh_p1_skip
    ubfx    r3, r9, #16, #8
    cbz     r3, .Ldh_p1_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #2]
    strb    r7, [r5, #1]
.Ldh_p1_skip:

    /* Pixel 2 <- pixels[5] */
    ldrb    r10, [r5, #2]
    cmp     r6, r10
    bls     .Ldh_p2_skip
    ubfx    r3, r9, #8, #8
    cbz     r3, .Ldh_p2_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #4]
    strb    r7, [r5, #2]
.Ldh_p2_skip:

    /* Pixel 3 <- pixels[4] */
    ldrb    r10, [r5, #3]
    cmp     r6, r10
    bls     .Ldh_p3_skip
    and     r3, r9, #0xFF
    cbz     r3, .Ldh_p3_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #6]
    strb    r7, [r5, #3]
.Ldh_p3_skip:

    /* Pixel 4 <- pixels[3] */
    ldrb    r10, [r5, #4]
    cmp     r6, r10
    bls     .Ldh_p4_skip
    lsr     r3, r8, #24
    cbz     r3, .Ldh_p4_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #8]
    strb    r7, [r5, #4]
.Ldh_p4_skip:

    /* Pixel 5 <- pixels[2] */
    ldrb    r10, [r5, #5]
    cmp     r6, r10
    bls     .Ldh_p5_skip
    ubfx    r3, r8, #16, #8
    cbz     r3, .Ldh_p5_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #10]
    strb    r7, [r5, #5]
.Ldh_p5_skip:

    /* Pixel 6 <- pixels[1] */
    ldrb    r10, [r5, #6]
    cmp     r6, r10
    bls     .Ldh_p6_skip
    ubfx    r3, r8, #8, #8
    cbz     r3, .Ldh_p6_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #12]
    strb    r7, [r5, #6]
.Ldh_p6_skip:

    /* Pixel 7 <- pixels[0] */
    ldrb    r10, [r5, #7]
    cmp     r6, r10
    bls     .Ldh_p7_skip
    and     r3, r8, #0xFF
    cbz     r3, .Ldh_p7_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #14]
    strb    r7, [r5, #7]
.Ldh_p7_skip:

    add     r0, r0, #8
    add     r4, r4, r11, lsl #1
    add     r5, r5, r11
    
    subs    r12, r12, #1
    bne     .Ldraw_fliph_loop
    
    pop     {r4-r11, pc}
.size DrawTile16_FlipH_asm, .-DrawTile16_FlipH_asm


/*
 * DrawTile16_VFlip_asm - Draw a complete 8x8 tile (V-flipped) with depth test
 * 
 * Same as Normal but decrements row pointer instead of incrementing.
 * StartLine should be 56 - original_StartLine for V_FLIP cases.
 */
.global DrawTile16_VFlip_asm
.type DrawTile16_VFlip_asm, %function
.thumb_func
DrawTile16_VFlip_asm:
    push    {r4-r11, lr}
    
    ldr     r12, [sp, #36]              @ LineCount
    add     r0, r0, r3                  @ bp = pCache + StartLine
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldr     r11, [r3, #GFX_OFF_PPL]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r1
    add     r4, r4, r1, lsl #1
    
.Ldraw_vflip_loop:
    ldm     r0, {r8, r9}                @ r8 = pixels[0-3], r9 = pixels[4-7]
    
    /* Pixel 0 */
    ldrb    r10, [r5, #0]
    cmp     r6, r10
    bls     .Ldv_p0_skip
    and     r3, r8, #0xFF
    cbz     r3, .Ldv_p0_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #0]
    strb    r7, [r5, #0]
.Ldv_p0_skip:

    /* Pixel 1 */
    ldrb    r10, [r5, #1]
    cmp     r6, r10
    bls     .Ldv_p1_skip
    ubfx    r3, r8, #8, #8
    cbz     r3, .Ldv_p1_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #2]
    strb    r7, [r5, #1]
.Ldv_p1_skip:

    /* Pixel 2 */
    ldrb    r10, [r5, #2]
    cmp     r6, r10
    bls     .Ldv_p2_skip
    ubfx    r3, r8, #16, #8
    cbz     r3, .Ldv_p2_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #4]
    strb    r7, [r5, #2]
.Ldv_p2_skip:

    /* Pixel 3 */
    ldrb    r10, [r5, #3]
    cmp     r6, r10
    bls     .Ldv_p3_skip
    lsr     r3, r8, #24
    cbz     r3, .Ldv_p3_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #6]
    strb    r7, [r5, #3]
.Ldv_p3_skip:

    /* Pixel 4 */
    ldrb    r10, [r5, #4]
    cmp     r6, r10
    bls     .Ldv_p4_skip
    and     r3, r9, #0xFF
    cbz     r3, .Ldv_p4_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #8]
    strb    r7, [r5, #4]
.Ldv_p4_skip:

    /* Pixel 5 */
    ldrb    r10, [r5, #5]
    cmp     r6, r10
    bls     .Ldv_p5_skip
    ubfx    r3, r9, #8, #8
    cbz     r3, .Ldv_p5_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #10]
    strb    r7, [r5, #5]
.Ldv_p5_skip:

    /* Pixel 6 */
    ldrb    r10, [r5, #6]
    cmp     r6, r10
    bls     .Ldv_p6_skip
    ubfx    r3, r9, #16, #8
    cbz     r3, .Ldv_p6_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #12]
    strb    r7, [r5, #6]
.Ldv_p6_skip:

    /* Pixel 7 */
    ldrb    r10, [r5, #7]
    cmp     r6, r10
    bls     .Ldv_p7_skip
    lsr     r3, r9, #24
    cbz     r3, .Ldv_p7_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #14]
    strb    r7, [r5, #7]
.Ldv_p7_skip:

    /* V_FLIP: decrement row pointer */
    sub     r0, r0, #8                  @ bp -= 8
    add     r4, r4, r11, lsl #1
    add     r5, r5, r11
    
    subs    r12, r12, #1
    bne     .Ldraw_vflip_loop
    
    pop     {r4-r11, pc}
.size DrawTile16_VFlip_asm, .-DrawTile16_VFlip_asm


/*
 * DrawTile16_FlipHV_asm - Draw a complete 8x8 tile (H+V flipped) with depth test
 * 
 * Combines H_FLIP pixel order with V_FLIP row direction.
 */
.global DrawTile16_FlipHV_asm
.type DrawTile16_FlipHV_asm, %function
.thumb_func
DrawTile16_FlipHV_asm:
    push    {r4-r11, lr}
    
    ldr     r12, [sp, #36]              @ LineCount
    add     r0, r0, r3                  @ bp = pCache + StartLine
    
    ldr     r3, =GFX
    ldr     r4, [r3, #GFX_OFF_S]
    ldr     r5, [r3, #GFX_OFF_DB]
    ldr     r11, [r3, #GFX_OFF_PPL]
    ldrb    r6, [r3, #GFX_OFF_Z1]
    ldrb    r7, [r3, #GFX_OFF_Z2]
    
    add     r5, r5, r1
    add     r4, r4, r1, lsl #1
    
.Ldraw_fliphv_loop:
    ldm     r0, {r8, r9}                @ r8 = pixels[0-3], r9 = pixels[4-7]
    
    /* H_FLIP: Screen[0] <- pixels[7], etc */
    
    /* Pixel 0 <- pixels[7] */
    ldrb    r10, [r5, #0]
    cmp     r6, r10
    bls     .Ldhv_p0_skip
    lsr     r3, r9, #24
    cbz     r3, .Ldhv_p0_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #0]
    strb    r7, [r5, #0]
.Ldhv_p0_skip:

    /* Pixel 1 <- pixels[6] */
    ldrb    r10, [r5, #1]
    cmp     r6, r10
    bls     .Ldhv_p1_skip
    ubfx    r3, r9, #16, #8
    cbz     r3, .Ldhv_p1_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #2]
    strb    r7, [r5, #1]
.Ldhv_p1_skip:

    /* Pixel 2 <- pixels[5] */
    ldrb    r10, [r5, #2]
    cmp     r6, r10
    bls     .Ldhv_p2_skip
    ubfx    r3, r9, #8, #8
    cbz     r3, .Ldhv_p2_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #4]
    strb    r7, [r5, #2]
.Ldhv_p2_skip:

    /* Pixel 3 <- pixels[4] */
    ldrb    r10, [r5, #3]
    cmp     r6, r10
    bls     .Ldhv_p3_skip
    and     r3, r9, #0xFF
    cbz     r3, .Ldhv_p3_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #6]
    strb    r7, [r5, #3]
.Ldhv_p3_skip:

    /* Pixel 4 <- pixels[3] */
    ldrb    r10, [r5, #4]
    cmp     r6, r10
    bls     .Ldhv_p4_skip
    lsr     r3, r8, #24
    cbz     r3, .Ldhv_p4_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #8]
    strb    r7, [r5, #4]
.Ldhv_p4_skip:

    /* Pixel 5 <- pixels[2] */
    ldrb    r10, [r5, #5]
    cmp     r6, r10
    bls     .Ldhv_p5_skip
    ubfx    r3, r8, #16, #8
    cbz     r3, .Ldhv_p5_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #10]
    strb    r7, [r5, #5]
.Ldhv_p5_skip:

    /* Pixel 6 <- pixels[1] */
    ldrb    r10, [r5, #6]
    cmp     r6, r10
    bls     .Ldhv_p6_skip
    ubfx    r3, r8, #8, #8
    cbz     r3, .Ldhv_p6_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #12]
    strb    r7, [r5, #6]
.Ldhv_p6_skip:

    /* Pixel 7 <- pixels[0] */
    ldrb    r10, [r5, #7]
    cmp     r6, r10
    bls     .Ldhv_p7_skip
    and     r3, r8, #0xFF
    cbz     r3, .Ldhv_p7_skip
    ldrh    r3, [r2, r3, lsl #1]
    strh    r3, [r4, #14]
    strb    r7, [r5, #7]
.Ldhv_p7_skip:

    /* V_FLIP: decrement row pointer */
    sub     r0, r0, #8                  @ bp -= 8
    add     r4, r4, r11, lsl #1
    add     r5, r5, r11
    
    subs    r12, r12, #1
    bne     .Ldraw_fliphv_loop
    
    pop     {r4-r11, pc}
.size DrawTile16_FlipHV_asm, .-DrawTile16_FlipHV_asm


/* End of file */
