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


/* End of file */
