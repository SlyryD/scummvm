#include "scumm_randomizer_standalone.h"
#include <iostream>

using namespace Scumm;

int main(int argc, char *argv[]) {
    std::string gameId = "MONKEY2";
    
    if (argc >= 2) {
        gameId = argv[1];
    }
    
    std::cout << "SCUMM Game Randomizer v1.0" << std::endl;
    std::cout << "Game: " << gameId << std::endl;
    std::cout << "=====================================" << std::endl;
    
    // Create the randomizer
    ScummRandomizer randomizer;
    
    // Set up paths
    std::string inputDir = "input/" + gameId;
    std::string outputDir = "output";
    
    std::cout << "Input directory: " << inputDir << std::endl;
    std::cout << "Output directory: " << outputDir << std::endl;
    
    // Initialize the randomizer
    if (!randomizer.initialize(inputDir, outputDir, gameId)) {
        std::cerr << "ERROR: Failed to initialize randomizer" << std::endl;
        return 1;
    }
    
    // Perform the randomization
    if (!randomizer.randomizeGame()) {
        std::cerr << "ERROR: Randomization failed" << std::endl;
        return 1;
    }
    
    std::cout << "Randomization completed successfully!" << std::endl;
    std::cout << "Randomized files written to: " << outputDir << std::endl;
    
    return 0;
}