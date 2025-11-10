#include "scumm_randomizer.h"
#include "common/debug.h"
#include "common/textconsole.h"
#include "base/main.h"

using namespace Scumm;

// Main entry point for the SCUMM randomizer tool
int main(int argc, char *argv[]) {
    // Initialize ScummVM common systems
    Common::String gameId = "MONKEY2";
    
    if (argc >= 2) {
        gameId = argv[1];
    }
    
    printf("SCUMM Game Randomizer v1.0\n");
    printf("Game: %s\n", gameId.c_str());
    printf("=====================================\n");
    
    // Create the randomizer
    ScummRandomizer randomizer;
    
    // Set up paths
    Common::Path inputDir("randomizer/input/" + gameId);
    Common::Path outputDir("randomizer/output");
    
    printf("Input directory: %s\n", inputDir.toString().c_str());
    printf("Output directory: %s\n", outputDir.toString().c_str());
    
    // Initialize the randomizer
    if (!randomizer.initialize(inputDir, outputDir, gameId)) {
        printf("ERROR: Failed to initialize randomizer\n");
        return 1;
    }
    
    // Perform the randomization
    if (!randomizer.randomizeGame()) {
        printf("ERROR: Randomization failed\n");
        return 1;
    }
    
    printf("Randomization completed successfully!\n");
    printf("Randomized files written to: %s\n", outputDir.toString().c_str());
    
    return 0;
}