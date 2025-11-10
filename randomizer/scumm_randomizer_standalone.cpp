#include "scumm_randomizer_standalone.h"
#include <iostream>
#include <cstdarg>
#include <random>
#include <algorithm>

using namespace Scumm;

// Little-endian reading functions 
static uint16 READ_LE_UINT16(const void *ptr) {
	const byte *b = (const byte *)ptr;
	return (uint16)(b[0] | (b[1] << 8));
}

static uint32 READ_LE_UINT32(const void *ptr) {
	const byte *b = (const byte *)ptr;
	return (uint32)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}

static uint16 READ_BE_UINT16(const void *ptr) {
	const byte *b = (const byte *)ptr;
	return (uint16)((b[0] << 8) | b[1]);
}

static uint32 READ_BE_UINT32(const void *ptr) {
	const byte *b = (const byte *)ptr;
	return (uint32)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
}

static void WRITE_LE_UINT16(void *ptr, uint16 value) {
	byte *b = (byte *)ptr;
	b[0] = (byte)(value & 0xFF);
	b[1] = (byte)((value >> 8) & 0xFF);
}

static void WRITE_LE_UINT32(void *ptr, uint32 value) {
	byte *b = (byte *)ptr;
	b[0] = (byte)(value & 0xFF);
	b[1] = (byte)((value >> 8) & 0xFF);
	b[2] = (byte)((value >> 16) & 0xFF);
	b[3] = (byte)((value >> 24) & 0xFF);
}

static void WRITE_BE_UINT32(void *ptr, uint32 value) {
	byte *b = (byte *)ptr;
	b[0] = (byte)((value >> 24) & 0xFF);
	b[1] = (byte)((value >> 16) & 0xFF);
	b[2] = (byte)((value >> 8) & 0xFF);
	b[3] = (byte)(value & 0xFF);
}

namespace Scumm {

// SCUMM resource type constants
enum ResType {
	rtInvalid = 0,
	rtFirst = 1,
	rtRoom = 1,
	rtScript = 2,
	rtCostume = 3,
	rtSound = 4,
	rtInventory = 5,
	rtCharset = 6,
	rtString = 7,
	rtVerb = 8,
	rtActorName = 9,
	rtBuffer = 10,
	rtScaleTable = 11,
	rtTemp = 12,
	rtFlObject = 13,
	rtMatrix = 14,
	rtBox = 15,
	rtObjectName = 16,
	rtRoomScripts = 17,
	rtRoomImage = 18,
	rtImage = 19,
	rtTalkie = 20,
	rtLast = 20
};

// Object flag constants
enum {
	OF_OWNER_MASK = 0x0F,
	OF_STATE_MASK = 0xF0,
	OF_STATE_SHL = 4
};

ScummRandomizer::ScummRandomizer() :
	_gameVersion(5), // Default to SCUMM v5 for MONKEY2
	_numGlobalObjects(0),
	_numRooms(0),
	_numScripts(0),
	_numSounds(0),
	_numCostumes(0),
	_randomSeed(12345) {
}

ScummRandomizer::~ScummRandomizer() {
	if (_indexFile.is_open())
		_indexFile.close();
	if (_outputIndexFile.is_open())
		_outputIndexFile.close();
}

bool ScummRandomizer::initialize(const std::string &inputDir, const std::string &outputDir, const std::string &gameId) {
	_inputDir = inputDir;
	_outputDir = outputDir;
	_gameId = gameId;
	
	logMessage("Initializing SCUMM Randomizer for %s", gameId.c_str());
	logMessage("Input directory: %s", inputDir.c_str());
	logMessage("Output directory: %s", outputDir.c_str());
	
	return true;
}

bool ScummRandomizer::randomizeGame() {
	logMessage("=== Starting SCUMM Game Randomization ===");
	
	// Step 1: Create output game directory and copy all files
	if (!copyAllGameFiles()) {
		logMessage("ERROR: Failed to copy game files");
		return false;
	}
	
	// Step 2: Read the index file
	if (!readIndexFile()) {
		logMessage("ERROR: Failed to read index file");
		return false;
	}
	
	// Step 3: Randomize the objects
	randomizeObjects();
	
	// Step 4: Write the new index file (overwrites the copied one)
	if (!writeIndexFile()) {
		logMessage("ERROR: Failed to write index file");
		return false;
	}
	
	logMessage("=== Randomization Complete ===");
	return true;
}

bool ScummRandomizer::readIndexFile() {
	// Construct the index filename (e.g., input/MONKEY2/MONKEY2.000)
	std::string indexPath = _inputDir + "/" + _gameId + "/" + _gameId + ".000";
	
	logMessage("Reading index file: %s", indexPath.c_str());
	
	_indexFile.open(indexPath, std::ios::binary);
	if (!_indexFile.is_open()) {
		logMessage("ERROR: Cannot open index file: %s", indexPath.c_str());
		return false;
	}
	
	// Check if file is encrypted (MONKEY2 uses 0x69 XOR encryption)
	char testBuffer[8];
	_indexFile.read(testBuffer, 8);
	_indexFile.seekg(0, std::ios::beg);
	
	// Decrypt with 0x69 to check for SCUMM magic
	bool isEncrypted = false;
	char decryptedTest[8];
	for (int i = 0; i < 8; i++) {
		decryptedTest[i] = testBuffer[i] ^ 0x69;
	}
	
	uint32 testTag = READ_BE_UINT32(decryptedTest);
	if (testTag == MKTAG('R','N','A','M') || testTag == MKTAG('L','E','C','F') || 
	    testTag == MKTAG('D','O','B','J') || testTag == MKTAG('D','R','O','O') || 
	    testTag == MKTAG('M','A','X','S')) {
		isEncrypted = true;
		logMessage("File is encrypted with 0x69 XOR");
	}
	
	// Read index blocks
	while (!_indexFile.eof()) {
		char blockHeader[8];
		_indexFile.read(blockHeader, 8);
		
		if (_indexFile.gcount() != 8)
			break;
		
		// Decrypt if needed
		if (isEncrypted) {
			for (int i = 0; i < 8; i++) {
				blockHeader[i] ^= 0x69;
			}
		}
			
		uint32 blocktype = READ_BE_UINT32(blockHeader);
		uint32 itemsize = READ_BE_UINT32(blockHeader + 4);
		
		logMessage("Reading index block '%s', size %d", tagToString(blocktype).c_str(), itemsize);
		
		if (!readIndexBlock(blocktype, itemsize, isEncrypted)) {
			logMessage("ERROR: Failed to read index block '%s'", tagToString(blocktype).c_str());
			return false;
		}
	}
	
	_indexFile.close();
	
	logMessage("Index file read successfully");
	logMessage("Objects: %d, Rooms: %d, Scripts: %d, Sounds: %d, Costumes: %d", 
		_numGlobalObjects, _numRooms, _numScripts, _numSounds, _numCostumes);
	
	return true;
}

bool ScummRandomizer::readIndexBlock(uint32 blocktype, uint32 itemsize, bool isEncrypted) {
	switch (blocktype) {
	case MKTAG('D','O','B','J'):
		logMessage("Reading global objects");
		return readGlobalObjects(isEncrypted);
		
	case MKTAG('D','R','O','O'):
	case MKTAG('D','I','R','R'):
		logMessage("Reading room list");
		return readResTypeList(rtRoom, isEncrypted);
		
	case MKTAG('D','S','C','R'):
	case MKTAG('D','I','R','S'):
		logMessage("Reading script list");
		return readResTypeList(rtScript, isEncrypted);
		
	case MKTAG('D','C','O','S'):
	case MKTAG('D','I','R','C'):
		logMessage("Reading costume list");
		return readResTypeList(rtCostume, isEncrypted);
		
	case MKTAG('D','S','O','U'):
	case MKTAG('D','I','R','N'):
		logMessage("Reading sound list");
		return readResTypeList(rtSound, isEncrypted);
		
	case MKTAG('M','A','X','S'):
		logMessage("Reading MAXS block");
		return readMAXS(itemsize, isEncrypted);
		
	case MKTAG('R','N','A','M'):
		logMessage("Skipping room names");
		skipBytes(itemsize - 8, isEncrypted);
		return true;
		
	case MKTAG('L','E','C','F'):
		logMessage("Reading LECF container");
		return readLECFContainer(itemsize, isEncrypted);
		
	default:
		logMessage("Skipping unknown block '%s'", tagToString(blocktype).c_str());
		skipBytes(itemsize - 8, isEncrypted);
		return true;
	}
}

bool ScummRandomizer::readGlobalObjects(bool isEncrypted) {
	char countBuffer[2];
	_indexFile.read(countBuffer, 2);
	
	if (isEncrypted) {
		for (int i = 0; i < 2; i++) {
			countBuffer[i] ^= 0x69;
		}
	}
	
	uint16 num = READ_LE_UINT16(countBuffer);
	_numGlobalObjects = num;
	
	logMessage("Reading %d global objects", num);
	
	_objects.resize(num);
	
	// Read owner table
	for (int i = 0; i < num; i++) {
		byte ownerState;
		_indexFile.read((char*)&ownerState, 1);
		if (isEncrypted) {
			ownerState ^= 0x69;
		}
		_objects[i].owner = ownerState & OF_OWNER_MASK;
		_objects[i].state = (ownerState & OF_STATE_MASK) >> OF_STATE_SHL;
	}
	
	// Read class data
	for (int i = 0; i < num; i++) {
		char classBuffer[4];
		_indexFile.read(classBuffer, 4);
		if (isEncrypted) {
			for (int j = 0; j < 4; j++) {
				classBuffer[j] ^= 0x69;
			}
		}
		_objects[i].classData = READ_LE_UINT32(classBuffer);
	}
	
	dumpObjectInfo();
	return true;
}

bool ScummRandomizer::readResTypeList(int resType, bool isEncrypted) {
	char countBuffer[2];
	_indexFile.read(countBuffer, 2);
	
	if (isEncrypted) {
		for (int i = 0; i < 2; i++) {
			countBuffer[i] ^= 0x69;
		}
	}
	
	uint16 num = READ_LE_UINT16(countBuffer);
	
	logMessage("Reading %d resources of type %d", num, resType);
	
	switch (resType) {
	case rtRoom:
		_numRooms = num;
		break;
	case rtScript:
		_numScripts = num;
		break;
	case rtSound:
		_numSounds = num;
		break;
	case rtCostume:
		_numCostumes = num;
		break;
	}
	
	// For now, just skip the resource entries
	skipBytes(num * 5, isEncrypted);
	
	return true;
}

bool ScummRandomizer::readMAXS(uint32 itemsize, bool isEncrypted) {
	// MAXS block contains max counts for various resources
	skipBytes(itemsize - 8, isEncrypted);
	return true;
}

bool ScummRandomizer::readLECFContainer(uint32 itemsize, bool isEncrypted) {
	// LECF is a container block - read sub-blocks within it
	uint32 remainingSize = itemsize - 8;
	while (remainingSize > 8) {
		char blockHeader[8];
		_indexFile.read(blockHeader, 8);
		
		if (isEncrypted) {
			for (int i = 0; i < 8; i++) {
				blockHeader[i] ^= 0x69;
			}
		}
		
		uint32 blocktype = READ_BE_UINT32(blockHeader);
		uint32 blocksize = READ_BE_UINT32(blockHeader + 4);
		
		logMessage("Reading LECF sub-block '%s', size %d", tagToString(blocktype).c_str(), blocksize);
		
		if (!readIndexBlock(blocktype, blocksize, isEncrypted)) {
			logMessage("ERROR: Failed to read LECF sub-block '%s'", tagToString(blocktype).c_str());
			return false;
		}
		
		remainingSize -= blocksize;
	}
	
	return true;
}

void ScummRandomizer::skipBytes(uint32 count, bool isEncrypted) {
	if (isEncrypted) {
		// If encrypted, we need to read and discard the bytes
		std::vector<char> buffer(count);
		_indexFile.read(buffer.data(), count);
	} else {
		_indexFile.seekg(count, std::ios::cur);
	}
}

void ScummRandomizer::randomizeObjects() {
	logMessage("=== Randomizing Objects ===");
	
	if (_objects.size() == 0) {
		logMessage("No objects to randomize");
		return;
	}
	
	// Randomize object ownership
	randomizeObjectOwnership();
	
	// Randomize object states  
	randomizeObjectStates();
	
	// Randomize object classes (partially)
	randomizeObjectClasses();
	
	logMessage("Object randomization complete");
	dumpObjectInfo();
}

void ScummRandomizer::randomizeObjectOwnership() {
	logMessage("Randomizing object ownership...");
	
	for (size_t i = 0; i < _objects.size(); i++) {
		// Keep some objects with their original owners (player=1, room=0)
		// Randomize others between different rooms/characters
		if (_objects[i].owner != 0 && _objects[i].owner != 1) {
			// Randomly assign to rooms 0-15 or characters 1-3
			_objects[i].owner = randomNumber(15);
		}
	}
	
	logMessage("Object ownership randomized");
}

void ScummRandomizer::randomizeObjectStates() {
	logMessage("Randomizing object states...");
	
	for (size_t i = 0; i < _objects.size(); i++) {
		// Object states are typically 0-15 (4 bits)
		// Be conservative and only modify some states
		if (randomNumber(2) == 0) { // 33% chance to modify
			_objects[i].state = randomNumber(15);
		}
	}
	
	logMessage("Object states randomized");
}

void ScummRandomizer::randomizeObjectClasses() {
	logMessage("Randomizing object classes...");
	
	for (size_t i = 0; i < _objects.size(); i++) {
		// Class data is complex - only modify some bits carefully
		// Preserve important class flags but randomize others
		uint32 originalClass = _objects[i].classData;
		
		// Flip some random bits in the lower 16 bits (less critical)
		if (randomNumber(3) == 0) { // 25% chance to modify
			uint16 randomBits = randomNumber(0xFFFF);
			_objects[i].classData = (originalClass & 0xFFFF0000) | (randomBits & 0x0000FFFF);
		}
	}
	
	logMessage("Object classes randomized");
}

bool ScummRandomizer::copyAllGameFiles() {
	// Create output game directory
	std::string outputGameDir = _outputDir + "/" + _gameId;
	
	logMessage("Creating output game directory: %s", outputGameDir.c_str());
	
	// Create directory (Note: This is a simple approach - in production you'd want proper directory creation)
	std::string mkdirCmd = "mkdir -p \"" + outputGameDir + "\"";
	if (system(mkdirCmd.c_str()) != 0) {
		logMessage("ERROR: Failed to create output directory: %s", outputGameDir.c_str());
		return false;
	}
	
	// Copy all files from input game directory to output game directory
	std::string inputGameDir = _inputDir + "/" + _gameId;
	std::string copyCmd = "cp \"" + inputGameDir + "\"/* \"" + outputGameDir + "\"/";
	
	logMessage("Copying all game files from %s to %s", inputGameDir.c_str(), outputGameDir.c_str());
	
	if (system(copyCmd.c_str()) != 0) {
		logMessage("ERROR: Failed to copy game files");
		return false;
	}
	
	logMessage("All game files copied successfully");
	return true;
}

bool ScummRandomizer::writeIndexFile() {
	std::string outputIndexPath = _outputDir + "/" + _gameId + "/" + _gameId + ".000";
	
	logMessage("Writing randomized index file: %s", outputIndexPath.c_str());
	
	_outputIndexFile.open(outputIndexPath, std::ios::binary);
	if (!_outputIndexFile.is_open()) {
		logMessage("ERROR: Cannot create output index file: %s", outputIndexPath.c_str());
		return false;
	}
	
	// Re-read the original index file to copy structure
	std::string indexPath = _inputDir + "/" + _gameId + "/" + _gameId + ".000";
	_indexFile.open(indexPath, std::ios::binary);
	if (!_indexFile.is_open()) {
		logMessage("ERROR: Cannot re-open index file: %s", indexPath.c_str());
		return false;
	}
	
	// Copy the index file, but replace object data
	while (!_indexFile.eof()) {
		char blockHeader[8];
		_indexFile.read(blockHeader, 8);
		
		if (_indexFile.gcount() != 8)
			break;
		
		uint32 blocktype = READ_BE_UINT32(blockHeader);
		uint32 itemsize = READ_BE_UINT32(blockHeader + 4);
		
		// Write block header
		_outputIndexFile.write(blockHeader, 8);
		
		if (blocktype == MKTAG('D','O','B','J')) {
			// Write our randomized object data
			writeGlobalObjects();
		} else {
			// Copy other blocks as-is
			std::vector<char> buffer(itemsize - 8);
			_indexFile.read(buffer.data(), itemsize - 8);
			_outputIndexFile.write(buffer.data(), itemsize - 8);
		}
	}
	
	_indexFile.close();
	_outputIndexFile.close();
	
	logMessage("Index file written successfully");
	return true;
}

bool ScummRandomizer::writeGlobalObjects() {
	logMessage("Writing %d randomized global objects", _objects.size());
	
	// Write object count
	char countBuffer[2];
	WRITE_LE_UINT16(countBuffer, _objects.size());
	_outputIndexFile.write(countBuffer, 2);
	
	// Write owner/state table
	for (size_t i = 0; i < _objects.size(); i++) {
		byte ownerState = (_objects[i].state << OF_STATE_SHL) | _objects[i].owner;
		_outputIndexFile.write((char*)&ownerState, 1);
	}
	
	// Write class data
	for (size_t i = 0; i < _objects.size(); i++) {
		char classBuffer[4];
		WRITE_LE_UINT32(classBuffer, _objects[i].classData);
		_outputIndexFile.write(classBuffer, 4);
	}
	
	return true;
}

std::string ScummRandomizer::tagToString(uint32 tag) {
	char str[5];
	str[0] = (char)(tag >> 24);
	str[1] = (char)(tag >> 16);
	str[2] = (char)(tag >> 8);
	str[3] = (char)(tag);
	str[4] = 0;
	return std::string(str);
}

void ScummRandomizer::dumpObjectInfo() {
	logMessage("=== Object Information ===");
	for (size_t i = 0; i < _objects.size() && i < 20; i++) { // Show first 20 objects
		logMessage("Object %zu: Owner=%d, State=%d, Class=0x%08x", 
			i, _objects[i].owner, _objects[i].state, _objects[i].classData);
	}
	if (_objects.size() > 20) {
		logMessage("... and %zu more objects", _objects.size() - 20);
	}
}

uint32 ScummRandomizer::randomNumber(uint32 max) {
	static std::mt19937 generator(_randomSeed);
	std::uniform_int_distribution<uint32> distribution(0, max);
	return distribution(generator);
}

void ScummRandomizer::logMessage(const char* format, ...) {
	va_list args;
	va_start(args, format);
	
	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), format, args);
	
	printf("[SCUMM-Randomizer] %s\n", buffer);
	fflush(stdout);
	
	va_end(args);
}

} // End of namespace Scumm