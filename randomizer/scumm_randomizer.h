#ifndef SCUMM_RANDOMIZER_H
#define SCUMM_RANDOMIZER_H

// Include ScummVM headers in the right order to avoid issues
#include "common/scummsys.h"
#include "common/endian.h"
#include "common/file.h"
#include "common/path.h"
#include "common/array.h"
#include "common/hashmap.h"
#include "common/random.h"
#include "common/str.h"

namespace Scumm {

// Forward declarations
class ScummEngine;

/**
 * SCUMM Game Object Randomizer
 * 
 * This class reads SCUMM index (.000) and data (.001) files,
 * randomizes object properties (ownership, state, classes),
 * and writes the modified data back to new files.
 * 
 * Uses ScummVM's internal file and data structures.
 */
class ScummRandomizer {
public:
	struct ObjectData {
		byte owner;
		byte state;
		uint32 classData;
	};

	struct ResourceEntry {
		uint32 offset;
		uint32 size;
		byte roomNo;
	};

private:
	// Input/output directories
	Common::Path _inputDir;
	Common::Path _outputDir;
	
	// Game data
	Common::String _gameId;
	int _gameVersion;
	
	// Resource data
	Common::Array<ObjectData> _objects;
	Common::HashMap<int, ResourceEntry> _rooms;
	Common::HashMap<int, ResourceEntry> _scripts;
	Common::HashMap<int, ResourceEntry> _sounds;
	Common::HashMap<int, ResourceEntry> _costumes;
	
	// File handles (using ScummVM's File class)
	Common::File _indexFile;
	Common::File _dataFile;
	Common::File _outputIndexFile;
	Common::File _outputDataFile;
	
	// Random number generator
	Common::RandomSource _randomSource;
	
	// Internal state
	int _numGlobalObjects;
	int _numRooms;
	int _numScripts;
	int _numSounds;
	int _numCostumes;

	// Optional Scumm engine instance used to access SCUMM internals
	ScummEngine *_engine;

public:
	ScummRandomizer();
	~ScummRandomizer();
	
	/**
	 * Initialize the randomizer with input and output directories
	 */
	bool initialize(const Common::Path &inputDir, const Common::Path &outputDir, const Common::String &gameId);
	
	/**
	 * Main entry point - randomizes the game files
	 */
	bool randomizeGame();
	
private:
	// Core functionality (based on ScummEngine methods)
	bool readIndexFile();
	bool readIndexBlock(uint32 blocktype, uint32 itemsize);
	bool readResTypeList(int resType);
	bool readGlobalObjects();
	bool readMAXS(uint32 itemsize);
	
	// Randomization
	void randomizeObjects();
	void randomizeObjectOwnership();
	void randomizeObjectStates();
	void randomizeObjectClasses();
	
	// Writing
	bool writeIndexFile();
	bool writeGlobalObjects();
	bool copyDataFile();
	
	// Utilities using ScummVM functions
	void skipBytes(uint32 count);
	Common::String tagToString(uint32 tag);
	
	// Debug
	void dumpObjectInfo();
	void logMessage(const char* format, ...);
};

} // End of namespace Scumm

#endif // SCUMM_RANDOMIZER_H