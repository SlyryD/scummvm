MODULE := base

MODULE_OBJS := \
	test_new_standards.o \
	main.o \
	commandLine.o \
	plugins.o \
	version.o

RANDOMIZER_MODULE_OBJS := \
	base/test_new_standards.o \
	randomizer/scumm_randomizer_tool.o \
	base/commandLine.o \
	base/plugins.o \
	base/version.o

# Include common rules
include $(srcdir)/rules.mk
