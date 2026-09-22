# 用法（工程放在 SDK 的 examples/wifi/sta/ 下时）：
#   make CHIP=bl616 BOARD=bl616dk
#   make flash CHIP=bl616 BOARD=bl616dk COMX=/dev/ttyUSB0
# 独立目录编译时显式传 SDK 路径：
#   make BL_SDK_BASE=/path/to/bouffalo_sdk CHIP=bl616 BOARD=bl616dk

SDK_DEMO_PATH ?= .
BL_SDK_BASE ?= $(SDK_DEMO_PATH)/../../../..

export BL_SDK_BASE

CHIP ?= bl616
BOARD ?= bl616dk
CROSS_COMPILE ?= riscv64-unknown-elf-

include $(BL_SDK_BASE)/project.build
