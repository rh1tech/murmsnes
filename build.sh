#!/bin/bash
rm -rf ./build
mkdir build
cd build

# Optional build-time overrides (defaults match CMakeLists.txt cache defaults)
: "${BOARD_VARIANT:=M1}"
: "${CPU_SPEED:=504}"
: "${PSRAM_SPEED:=166}"
: "${MURMSNES_PROFILE:=ON}"
: "${MURMSNES_FAST_MODE:=ON}"

cmake \
	-DPICO_PLATFORM=rp2350 \
	-DMURMSNES_PROFILE=${MURMSNES_PROFILE} \
	-DMURMSNES_FAST_MODE=${MURMSNES_FAST_MODE} \
	-DBOARD_VARIANT=${BOARD_VARIANT} \
	-DCPU_SPEED=${CPU_SPEED} \
	-DPSRAM_SPEED=${PSRAM_SPEED} \
	..
make -j4
