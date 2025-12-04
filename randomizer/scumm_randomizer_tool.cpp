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

// FIXME: Avoid using printf
#define FORBIDDEN_SYMBOL_EXCEPTION_printf
#define FORBIDDEN_SYMBOL_EXCEPTION_fprintf
#define FORBIDDEN_SYMBOL_EXCEPTION_stderr

#include "scumm_randomizer.h"
#include "common/path.h"
#include "common/str.h"
#include <cstdlib>

using namespace Scumm;

void printUsage() {
	printf("ScummVM SCUMM Game Randomizer\n");
	printf("=============================\n");
	printf("\n");
	printf("Usage: scumm_randomizer_tool <game_id> [input_dir] [output_dir]\n");
	printf("\n");
	printf("Parameters:\n");
	printf("  game_id    - SCUMM game identifier (e.g., MONKEY2, INDY3, LOOM)\n");
	printf("  input_dir  - Directory containing original .000/.001 files\n");
	printf("              (default: ./randomizer/input/<game_id>)\n");
	printf("  output_dir - Directory to write randomized files\n");
	printf("              (default: ./randomizer/output)\n");
	printf("\n");
	printf("Example:\n");
	printf("  scumm_randomizer_tool MONKEY2\n");
	printf("  scumm_randomizer_tool MONKEY2 /path/to/monkey2/files /path/to/output\n");
	printf("\n");
}

extern "C" int scummvm_main(int argc, const char * const argv[]) {
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
	
	printf("ScummVM SCUMM Game Randomizer\n");
	printf("=============================\n");
	printf("Game ID: %s\n", gameId.c_str());
	printf("Input:   %s\n", inputDir.toString().c_str());
	printf("Output:  %s\n", outputDir.toString().c_str());
	printf("\n");
	
	// Create and initialize the randomizer
	ScummRandomizer randomizer;
	
	if (!randomizer.initialize(inputDir, outputDir, gameId)) {
		fprintf(stderr, "ERROR: Failed to initialize randomizer\n");
		return 1;
	}
	
	// Run the randomization
	if (!randomizer.randomizeGame()) {
		fprintf(stderr, "ERROR: Failed to randomize game\n");
		return 1;
	}
	
	printf("\n");
	printf("Randomization completed successfully!\n");
	printf("You can now copy the files from the output directory\n");
	printf("to your ScummVM game directory.\n");
	
	return 0;
}