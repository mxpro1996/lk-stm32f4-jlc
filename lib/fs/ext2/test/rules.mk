LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_DEPS += kernel
MODULE_DEPS += lib/bio
MODULE_DEPS += lib/cmdline
MODULE_DEPS += lib/fs
MODULE_DEPS += lib/fs/ext2
MODULE_DEPS += lib/fs/memfs
MODULE_DEPS += lib/unittest

MODULE_SRCS += $(LOCAL_DIR)/ext2_tests.c

include make/module.mk
