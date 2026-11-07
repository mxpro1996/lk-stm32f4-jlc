LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_DEPS += \
	lib/bio \
	lib/cksum

MODULE_SRCS += \
	$(LOCAL_DIR)/gpt.c \
	$(LOCAL_DIR)/partition.c

MODULE_OPTIONS := test

include make/module.mk
