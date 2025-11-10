/**
 * ScummVM SCUMM Game Randomizer
 * 
 * This is a standalone tool that reads SCUMM index (.000) and data (.001) files,
 * randomizes game objects (ownership, states, classes), and writes modified
 * files that can be used with ScummVM.
 * 
 * Usage: scumm_randomizer_tool <game_id> [input_dir] [output_dir]
 * 
 * Example: scumm_randomizer_tool MONKEY2 ./randomizer/input/MONKEY2 ./randomizer/output
 */

#include "scumm_randomizer.h"
#include "common/path.h"
#include "common/str.h"
#include <iostream>
#include <cstdlib>

using namespace Scumm;

void printUsage() {
	std::cout << "ScummVM SCUMM Game Randomizer" << std::endl;
	std::cout << "=============================" << std::endl;
	std::cout << std::endl;
	std::cout << "Usage: scumm_randomizer_tool <game_id> [input_dir] [output_dir]" << std::endl;
	std::cout << std::endl;
	std::cout << "Parameters:" << std::endl;
	std::cout << "  game_id    - SCUMM game identifier (e.g., MONKEY2, INDY3, LOOM)" << std::endl;
	std::cout << "  input_dir  - Directory containing original .000/.001 files" << std::endl;
	std::cout << "              (default: ./randomizer/input/<game_id>)" << std::endl;
	std::cout << "  output_dir - Directory to write randomized files" << std::endl;
	std::cout << "              (default: ./randomizer/output)" << std::endl;
	std::cout << std::endl;
	std::cout << "Example:" << std::endl;
	std::cout << "  scumm_randomizer_tool MONKEY2" << std::endl;
	std::cout << "  scumm_randomizer_tool MONKEY2 /path/to/monkey2/files /path/to/output" << std::endl;
	std::cout << std::endl;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		printUsage();
		return 1;
	}
	
	// Parse command line arguments
	Common::String gameId = argv[1];
	
	// Convert to uppercase for consistency
	gameId.toUppercase();
	
	// Determine input directory
	Common::Path inputDir;
	if (argc >= 3) {
		inputDir = Common::Path(argv[2]);
	} else {
		// Default to ./randomizer/input/<game_id>
		inputDir = Common::Path("./randomizer/input").appendComponent(gameId);
	}
	
	// Determine output directory
	Common::Path outputDir;
	if (argc >= 4) {
		outputDir = Common::Path(argv[3]);
	} else {
		// Default to ./randomizer/output
		outputDir = Common::Path("./randomizer/output");
	}
	
	std::cout << "ScummVM SCUMM Game Randomizer" << std::endl;
	std::cout << "=============================" << std::endl;
	std::cout << "Game ID: " << gameId.c_str() << std::endl;
	std::cout << "Input:   " << inputDir.toString().c_str() << std::endl;
	std::cout << "Output:  " << outputDir.toString().c_str() << std::endl;
	std::cout << std::endl;
	
	// Create and initialize the randomizer
	ScummRandomizer randomizer;
	
	if (!randomizer.initialize(inputDir, outputDir, gameId)) {
		std::cerr << "ERROR: Failed to initialize randomizer" << std::endl;
		return 1;
	}
	
	// Run the randomization
	if (!randomizer.randomizeGame()) {
		std::cerr << "ERROR: Failed to randomize game" << std::endl;
		return 1;
	}
	
	std::cout << std::endl;
	std::cout << "Randomization completed successfully!" << std::endl;
	std::cout << "You can now copy the files from the output directory" << std::endl;
	std::cout << "to your ScummVM game directory." << std::endl;
	
	return 0;
}