LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

# Header only: LK's own C++ library, namespace lk, headers under include/lktl/.
# The module carries the include path, the dependency on lib/libcpp and the tests.

MODULE_DEPS += lib/libcpp

MODULE_OPTIONS := test

include make/module.mk
