MODULE := base

MODULE_OBJS := \
	test_new_standards.o \
	main.o \
	commandLine.o \
	plugins.o \
	version.o

# Include common rules
include $(srcdir)/rules.mk

# base lib compiled with main_randomizer.o instead of main.o
MODULE_OBJS_BASE_RANDOMIZER := $(addprefix $(MODULE)/,$(patsubst main.o,main_randomizer.o,$(MODULE_OBJS)))

MODULE_LIB_BASE_RANDOMIZER := $(MODULE)/libbase_randomizer.a
