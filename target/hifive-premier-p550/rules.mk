LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

PLATFORM := eic7700

# HF106-000 has 16GB, HF106-001 has 32GB; the FDT from U-Boot reports the real size
MEMSIZE ?= 0x400000000
