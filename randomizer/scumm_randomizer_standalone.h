#ifndef SCUMM_RANDOMIZER_H
#define SCUMM_RANDOMIZER_H

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>

namespace Scumm {

// SCUMM tag creation macro
#define MKTAG(a0,a1,a2,a3) ((uint32_t)((a3) | ((a2) << 8) | ((a1) << 16) | ((a0) << 24)))

// Basic types
typedef uint8_t byte;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

struct ObjectData {
	byte owner;          // Object owner (player, room, etc.)
	byte state;          // Object state (4 bits)
	uint32 classData;    // Object class information
};

struct ResourceEntry {
	byte room;           // Room number for this resource
	uint32 offset;       // Offset in data file
};

class ScummRandomizer {
public:
	ScummRandomizer();
	~ScummRandomizer();
	
	bool initialize(const std::string &inputDir, const std::string &outputDir, const std::string &gameId);
	bool randomizeGame();
	
private:
	// File operations
	bool readIndexFile();
	bool readIndexBlock(uint32 blocktype, uint32 itemsize, bool isEncrypted);
	bool readGlobalObjects(bool isEncrypted);
	bool readResTypeList(int resType, bool isEncrypted);
	bool readMAXS(uint32 itemsize, bool isEncrypted);
	bool readLECFContainer(uint32 itemsize, bool isEncrypted);
	
	// Randomization
	void randomizeObjects();
	void randomizeObjectOwnership();
	void randomizeObjectStates();
	void randomizeObjectClasses();
	
	// Output
	bool writeIndexFile();
	bool writeGlobalObjects();
	bool copyDataFile();
	
	// Utilities
	void skipBytes(uint32 count, bool isEncrypted);
	std::string tagToString(uint32 tag);
	void dumpObjectInfo();
	void logMessage(const char* format, ...);
	uint32 randomNumber(uint32 max);
	
	// File streams
	std::ifstream _indexFile;
	std::ifstream _dataFile;
	std::ofstream _outputIndexFile;
	std::ofstream _outputDataFile;
	
	// Paths
	std::string _inputDir;
	std::string _outputDir;
	std::string _gameId;
	
	// Game data
	int _gameVersion;
	uint16 _numGlobalObjects;
	uint16 _numRooms;
	uint16 _numScripts;
	uint16 _numSounds;
	uint16 _numCostumes;
	
	// Object data
	std::vector<ObjectData> _objects;
	std::vector<ResourceEntry> _rooms;
	std::vector<ResourceEntry> _scripts;
	std::vector<ResourceEntry> _sounds;
	std::vector<ResourceEntry> _costumes;
	
	// Random seed
	uint32 _randomSeed;
};

} // End of namespace Scumm

#endif // SCUMM_RANDOMIZER_H