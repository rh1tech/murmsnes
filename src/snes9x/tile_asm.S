/* This file is part of Snes9x. See LICENSE file. */
/* Assembly-optimized tile rendering functions for ARM Cortex-M33 (RP2350) */

.syntax unified
.cpu cortex-m33
.thumb

/* Put in time-critical section for RAM execution */
.section .time_critical.snes9x_tile_asm, "ax", %progbits

/* External symbols */
.extern GFX

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
.equ GFX_OFF_Z1, 64         @ Z1 depth compare (uint8_t)
.equ GFX_OFF_Z2, 65         @ Z2 depth write (uint8_t)

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


/* End of file */
