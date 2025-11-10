# SCUMM Randomizer Makefile

# Compiler settings
CXX := g++
CXXFLAGS := -std=c++11 -Wall -Wextra -O2 -g
INCLUDES := -I.. -I../engines -I../common -I../backends -I../graphics
DEFINES := -DHAVE_CONFIG_H -DENABLE_SCUMM -DSCUMM_LITTLE_ENDIAN

# Target executable
TARGET := scumm_randomizer

# Source files
SOURCES := main.cpp scumm_randomizer.cpp
OBJECTS := $(SOURCES:.cpp=.o)

# System libraries
LIBS := -lz -lpng -ljpeg -lpthread

# Default target
all: $(TARGET)

# Build the executable
$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# Compile source files
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)

# Phony targets
.PHONY: all clean

# Dependencies
main.o: main.cpp scumm_randomizer.h
scumm_randomizer.o: scumm_randomizer.cpp scumm_randomizer.h

# Compile ScummVM common sources (with different output location)
file.o: $(SCUMMVM_ROOT)/common/file.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

fs.o: $(SCUMMVM_ROOT)/common/fs.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

path.o: $(SCUMMVM_ROOT)/common/path.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

str.o: $(SCUMMVM_ROOT)/common/str.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

random.o: $(SCUMMVM_ROOT)/common/random.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

util.o: $(SCUMMVM_ROOT)/common/util.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

debug.o: $(SCUMMVM_ROOT)/common/debug.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

textconsole.o: $(SCUMMVM_ROOT)/common/textconsole.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(DEFINES) -c $< -o $@

# Clean up
clean:
	@echo "Cleaning..."
	rm -f $(RANDOMIZER_OBJS) $(SCUMMVM_OBJS) $(TARGET)

# Test run with MONKEY2
test: $(TARGET)
	@echo "Testing randomizer with MONKEY2..."
	./$(TARGET) MONKEY2

# Install (copy to parent directory)
install: $(TARGET)
	@echo "Installing to parent directory..."
	cp $(TARGET) $(SCUMMVM_ROOT)/

# Help
help:
	@echo "SCUMM Game Randomizer Makefile"
	@echo "=============================="
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build the randomizer tool (default)"
	@echo "  clean   - Remove built files"
	@echo "  test    - Build and test with MONKEY2"
	@echo "  install - Copy built tool to parent directory"
	@echo "  help    - Show this help"
	@echo ""
	@echo "Usage after building:"
	@echo "  ./$(TARGET) MONKEY2"
	@echo "  ./$(TARGET) MONKEY2 /path/to/input /path/to/output"

.PHONY: all clean test install help