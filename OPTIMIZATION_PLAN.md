# MURMSNES Performance Optimization Plan

## Current Performance Analysis (504 MHz, 166 MHz PSRAM)

### Frame Budget
- **Target**: 16,667 µs (60 fps)
- **Actual rendered frame**: 20,700-27,000 µs (**124-162% over budget**)
- **Actual skipped frame**: 12,000-15,000 µs (**72-90% of budget**)

### Time Breakdown (Rendered Frame)

| Component | Time (µs) | % of Frame | Priority |
|-----------|-----------|------------|----------|
| **BG1 layer (r1)** | 3,750-4,250 | 25% | 🔴 #1 |
| **BG0 layer (r0)** | 3,240-3,300 | 20% | 🔴 #2 |
| **CPU emulation (emuS base)** | 12,000-15,000 | 72-90% | 🔴 #3 |
| Sprites (ro) | 114-1,500 | 1-9% | 🟡 |
| BG2 layer (r2) | 1,200-1,280 | 7% | 🟡 |
| Z-buffer clear (uz) | 96-172 | 1% | ✅ |
| Color math (uMath) | 860-920 | 5% | 🟡 |
| Subscreen (uSub) | 1,100-1,800 | 7-11% | 🟡 |
| Audio mix | 350-650 | 2-4% | ✅ |
| Audio pack | 22-25 | <1% | ✅ |

### Key Observations

1. **Tile conversions are extremely high**: 6,000-32,000 per second
   - Each ConvertTile call decodes planar VRAM data
   - Game uses Mode 1/Mode 3 with 2-3 active BG layers

2. **BG rendering dominates**: 45% of frame time on BG0+BG1 alone
   - DrawBackground() → DrawTilePtr() → WRITE_4PIXELS16
   - 32 tiles × 239 lines × 2 layers = ~15,000+ tile calls/frame

3. **Even skipped frames are slow**: 12-15ms just for CPU/APU
   - S9xMainLoop() core emulation is the base cost
   - Memory access patterns may be hurting cache

---

## Optimization Plan (Priority Order)

### Phase 1: Background Layer Rendering (Target: -4,500 µs)

**Problem**: BG0+BG1 = 7,000 µs (42% of budget)

#### 1.1 Skip Disabled Layers Earlier
Currently we set up layer parameters even if layer is disabled.

```c
// In DrawBackground() - add early exit
if (!(GFX.r212c & (1 << bg)) && !(GFX.r212d & (1 << bg)))
    return;  // Layer not enabled on main or sub screen
```

#### 1.2 Batch Tile Rendering
Instead of calling DrawTilePtr() for each tile, batch 4-8 tiles:
- Pre-fetch tile addresses for entire scanline
- Prefetch VRAM tile data
- Reduce function call overhead

#### 1.3 Optimize DrawTilePtr Inner Loop
Current: 4 pixels at a time with individual depth checks
Improvement: Check if entire tile passes depth (common case), then bulk write

```c
// Fast path: if all 8 depth values pass, skip individual checks
uint64_t depth8 = *(uint64_t*)(Depth);
if ((depth8 & 0x....) < Z1_repeated) {
    // All pass: bulk write
    memcpy(Screen, &colors, 16);  // 8 × 16-bit
    memset(Depth, Z2, 8);
}
```

---

### Phase 2: Tile Cache Optimization (Target: -2,000 µs)

**Problem**: 6,000-32,000 tile conversions/second

#### 2.1 Better Tile Dirty Tracking
Current: Re-convert tiles on every frame
Improvement: Track VRAM writes more precisely

```c
// In S9xSetPPU / VRAM write handler
void InvalidateTileCache(uint16_t addr) {
    uint32_t tile_idx = (addr >> 4) & TILE_CACHE_MASK;
    IPPU.TileCached[tile_idx] = 0;  // Mark invalid
}
```

#### 2.2 Lazy Tile Conversion
Don't convert until actually rendered:
- Many tiles in VRAM are never displayed
- Convert on first DrawTile access

#### 2.3 Assembly ConvertTile (Already Exists)
Verify `ConvertTile_opt_8bpp` is being used and optimize further.

---

### Phase 3: CPU Emulation Core (Target: -4,000 µs)

**Problem**: 12,000-15,000 µs even when not rendering

#### 3.1 Memory Access Fast Path
Current `S9xGetByte_asm` handles common ROM/RAM case in assembly.
Verify it's being used and inline more aggressively.

#### 3.2 Opcode Handler Inlining
Most-executed opcodes should be fully inlined in S9xMainLoop:
- LDA, STA, LDX, LDY, STX, STY (loads/stores)
- BNE, BEQ, BPL, BMI (branches)
- JSR, RTS (calls)

#### 3.3 SPC700 (APU) Optimization
APU runs in sync with CPU - each instruction cycles both.
Consider:
- Batch APU updates (run N cycles at once)
- Skip APU on frames where audio buffer is full

---

### Phase 4: Subscreen/Color Math (Target: -1,500 µs)

**Problem**: uSub + uMath = 2,000-2,700 µs when active

#### 4.1 Disable Subscreen When Unused
If r212d == 0, skip entire subscreen rendering pass.

#### 4.2 Simplify Color Math
`SIMPLE_COLOR_MATH=1` is already defined, verify it's active.
Color math with fixed color is simpler than screen-to-screen blending.

---

### Phase 5: Z-Buffer Optimization (Target: -100 µs)

**Problem**: uz = 96-172 µs per frame

#### 5.1 Lazy Z-Clear
Only clear Z-buffer lines that will be rendered.
Skip if entire screen is Mode 7 (no depth sorting).

#### 5.2 SIMD Z-Clear
Use word-aligned memset or DMA for Z-buffer clear.

---

## Implementation Priority

| Phase | Target Savings | Effort | Impact |
|-------|---------------|--------|--------|
| 1.3 | -2,500 µs | Medium | High |
| 2.1-2.2 | -2,000 µs | Medium | High |
| 3.1-3.2 | -3,000 µs | High | Critical |
| 1.1 | -500 µs | Low | Medium |
| 4.1 | -1,000 µs | Low | Medium |
| 1.2 | -1,500 µs | High | Medium |
| 3.3 | -1,000 µs | Medium | Medium |

---

## Completed Optimizations

### ✅ 8-Pixel Row Functions (Phase 1.3 - Partial)
**Date**: Current
**Files Modified**:
- `src/snes9x/tile_asm.S` - Added `WRITE_8PIXELS16_OPAQUE_ROW_asm` and `WRITE_8PIXELS16_FLIPPED_OPAQUE_ROW_asm`
- `src/snes9x/tile.h` - Added `RENDER_TILE_OPAQUE_8PIX` macro
- `src/snes9x/tile.c` - Updated `DrawTile16()` to use new macro

**Improvement**:
- Reduced function call overhead by 50% for opaque tiles
- Each tile row now makes 1 call instead of 2
- Estimated savings: ~4ms per frame (theoretical)

**Before**: 
```
For each tile row: 
  WRITE_4PIXELS16_OPAQUE_asm(Offset, bp, colors)     // 4 pixels
  WRITE_4PIXELS16_OPAQUE_asm(Offset+4, bp+4, colors) // 4 pixels
```

**After**:
```
For each tile row:
  WRITE_8PIXELS16_OPAQUE_ROW_asm(Offset, bp, colors) // 8 pixels in one call
```

---

## Quick Wins (Implement First)

1. **Skip disabled BG layers early** - 10 lines of code
2. **Skip subscreen if r212d == 0** - 5 lines of code
3. ~~Verify assembly tile functions are being called~~ ✅ Verified - in SRAM at 0x2000xxxx
4. **Add prefetch hints to DrawBackground loop** - 10 lines of code

---

## Build Verification Commands

```bash
# Build with profiling
MURMSNES_PROFILE=ON ./build.sh

# Check if assembly functions are linked
arm-none-eabi-nm build/murmsnes.elf | grep -E "WRITE_4PIXELS|ConvertTile_opt"

# Disassemble hot functions
arm-none-eabi-objdump -d build/murmsnes.elf | grep -A 50 "DrawTile16"
```

---

## Success Metrics

| Metric | Current | Target | Stretch |
|--------|---------|--------|---------|
| emuR (rendered frame) | 20,700 µs | 14,000 µs | 10,000 µs |
| emuS (skipped frame) | 12,000 µs | 10,000 µs | 8,000 µs |
| rend_fps | 20 | 30 | 40 |
| emu_fps | 50-60 | 60 stable | 60 stable |
| tilec/sec | 6,000-32,000 | 2,000-10,000 | 1,000-5,000 |
