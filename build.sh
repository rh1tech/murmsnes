#!/bin/bash
rm -rf ./build
mkdir build
cd build

# Optional build-time overrides (defaults match CMakeLists.txt cache defaults)
: "${BOARD_VARIANT:=M1}"
: "${CPU_SPEED:=378}"
: "${PSRAM_SPEED:=133}"

cmake \
	-DPICO_PLATFORM=rp2350 \
	-DMURMSNES_PROFILE=${MURMSNES_PROFILE:-OFF} \
	-DBOARD_VARIANT=${BOARD_VARIANT} \
	-DCPU_SPEED=${CPU_SPEED} \
	-DPSRAM_SPEED=${PSRAM_SPEED} \
	..
make -j4
