#!/bin/bash

# Test script for SCUMM Game Randomizer
# This script builds the randomizer tool and tests it with MONKEY2 files

set -e  # Exit on any error

echo "SCUMM Game Randomizer Test Script"
echo "================================="
echo

# Check if we're in the right directory
if [ ! -f "Makefile" ]; then
    echo "ERROR: Please run this script from the randomizer directory"
    exit 1
fi

# Check if MONKEY2 input files exist
if [ ! -d "input/MONKEY2" ]; then
    echo "ERROR: MONKEY2 input directory not found"
    echo "Please ensure input/MONKEY2/ contains the original game files"
    exit 1
fi

if [ ! -f "input/MONKEY2/MONKEY2.000" ] || [ ! -f "input/MONKEY2/MONKEY2.001" ]; then
    echo "ERROR: MONKEY2.000 or MONKEY2.001 not found in input/MONKEY2/"
    echo "Please copy the original game files to input/MONKEY2/"
    exit 1
fi

echo "✓ Input files found"

# Create output directory if it doesn't exist
mkdir -p output
echo "✓ Output directory ready"

# Clean any previous builds
echo "Cleaning previous builds..."
make clean

# Build the randomizer tool
echo "Building randomizer tool..."
make all

if [ ! -f "scumm_randomizer_tool" ]; then
    echo "ERROR: Failed to build randomizer tool"
    exit 1
fi

echo "✓ Randomizer tool built successfully"

# Run the randomizer
echo
echo "Running randomizer on MONKEY2..."
echo "================================"
./scumm_randomizer_tool MONKEY2

# Check if output files were created
if [ ! -f "output/MONKEY2.000" ] || [ ! -f "output/MONKEY2.001" ]; then
    echo "ERROR: Output files not created"
    exit 1
fi

echo
echo "✓ Randomization completed successfully!"
echo
echo "Output files created:"
echo "  output/MONKEY2.000 ($(du -h output/MONKEY2.000 | cut -f1) - randomized index)"
echo "  output/MONKEY2.001 ($(du -h output/MONKEY2.001 | cut -f1) - data copy)"
echo
echo "You can now copy these files to your ScummVM game directory"
echo "and test them with ScummVM."
echo
echo "To test with ScummVM:"
echo "1. Backup your original MONKEY2.000 and MONKEY2.001 files"
echo "2. Copy output/MONKEY2.000 and output/MONKEY2.001 to your game directory"
echo "3. Run ScummVM and start Monkey Island 2"
echo "4. Observe the randomized object ownership and states in the game"
echo