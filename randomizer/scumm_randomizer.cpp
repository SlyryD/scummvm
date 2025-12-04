#include "scumm_randomizer.h"
#include "base/main.h"
#include "base/plugins.h"
#include "common/debug.h"
#include "common/textconsole.h"
#include "common/util.h"
#include "engines/game.h"
#include "engines/scumm/metaengine.h"
#include "engines/scumm/scumm.h"
#include <cstdarg>
#include <cstdlib>

namespace Scumm {

ScummRandomizer::ScummRandomizer() : _randomSource("scumm_randomizer"),
									 _gameVersion(5), // Default to SCUMM v5 for MONKEY2
									 _numGlobalObjects(0),
									 _numRooms(0),
									 _numScripts(0),
									 _numSounds(0),
									 _numCostumes(0) {
	ScummMetaEngine metaEngine;
	OSystem *system = ::g_system; // Get global OSystem pointer
	Common::String engine { metaEngine.getName() };
	PlainGameDescriptor pgd = PlainGameDescriptor::of("monkey2", "Monkey Island 2: LeChuck's Revenge");
	DetectedGame detectedGame { engine, pgd };
	Common::Error err = metaEngine.createInstance(
		system,
		reinterpret_cast<Engine **>(&_engine),
		detectedGame,
		nullptr);
	if (err.getCode() != Common::kNoError) {
		logMessage("ERROR: Failed to create ScummEngine instance");
		_engine = nullptr;
	} else {
		g_scumm = _engine; // Set global pointer for SCUMM engine
	}
}

ScummRandomizer::~ScummRandomizer() {
	if (_indexFile.isOpen())
		_indexFile.close();
	if (_dataFile.isOpen())
		_dataFile.close();
	if (_outputIndexFile.isOpen())
		_outputIndexFile.close();
	if (_outputDataFile.isOpen())
		_outputDataFile.close();

	// Clean up engine instance if created
	if (_engine) {
		delete _engine;
		_engine = nullptr;
		g_scumm = nullptr;
	}
}

bool ScummRandomizer::initialize(const Common::Path &inputDir, const Common::Path &outputDir, const Common::String &gameId) {
	_inputDir = inputDir;
	_outputDir = outputDir;
	_gameId = gameId;

	logMessage("Initializing SCUMM Randomizer for %s", gameId.c_str());
	logMessage("Input directory: %s", inputDir.toString().c_str());
	logMessage("Output directory: %s", outputDir.toString().c_str());

	// Initialize a ScummEngine so we can reuse parsing/reading helpers.
	if (_engine) {
		Common::Error err = _engine->init();
		if (err.getCode() != Common::kNoError) {
			logMessage("ERROR: ScummEngine::init failed");
			return false;
		}
	}

	return true;
}

bool ScummRandomizer::randomizeGame() {
	logMessage("=== Starting SCUMM Game Randomization ===");

	// Step 1: Read the index file
	if (!readIndexFile()) {
		logMessage("ERROR: Failed to read index file");
		return false;
	}

	// Step 2: Randomize the objects
	randomizeObjects();

	// Step 3: Write the new index file
	if (!writeIndexFile()) {
		logMessage("ERROR: Failed to write index file");
		return false;
	}

	// Step 4: Copy the data file
	if (!copyDataFile()) {
		logMessage("ERROR: Failed to copy data file");
		return false;
	}

	logMessage("=== Randomization Complete ===");
	return true;
}

bool ScummRandomizer::readIndexFile() {
	// Construct the index filename (e.g., MONKEY2.000)
	Common::Path indexPath = _inputDir.appendComponent(_gameId + ".000");

	logMessage("Reading index file: %s", indexPath.toString().c_str());

	if (!_indexFile.open(indexPath)) {
		logMessage("ERROR: Cannot open index file: %s", indexPath.toString().c_str());
		return false;
	}

	// For SCUMM v5, first read resource counts
	if (_gameVersion <= 5) {
		// Figure out the sizes of various resources
		while (!_indexFile.eos() && !_indexFile.err()) {
			uint32 blocktype = _indexFile.readUint32BE();
			uint32 itemsize = _indexFile.readUint32BE();

			if (_indexFile.eos() || _indexFile.err())
				break;

			switch (blocktype) {
			case MKTAG('D', 'O', 'B', 'J'):
				_numGlobalObjects = _indexFile.readUint16LE();
				itemsize -= 2;
				break;
			case MKTAG('D', 'R', 'O', 'O'):
				_numRooms = _indexFile.readUint16LE();
				itemsize -= 2;
				break;
			case MKTAG('D', 'S', 'C', 'R'):
				_numScripts = _indexFile.readUint16LE();
				itemsize -= 2;
				break;
			case MKTAG('D', 'C', 'O', 'S'):
				_numCostumes = _indexFile.readUint16LE();
				itemsize -= 2;
				break;
			case MKTAG('D', 'S', 'O', 'U'):
				_numSounds = _indexFile.readUint16LE();
				itemsize -= 2;
				break;
			default:
				break;
			}
			_indexFile.seek(itemsize - 8, SEEK_CUR);
		}
		_indexFile.seek(0, SEEK_SET);
	}

	// Now read index blocks
	while (!_indexFile.eos() && !_indexFile.err()) {
		uint32 blocktype = _indexFile.readUint32BE();
		uint32 itemsize = _indexFile.readUint32BE();

		if (_indexFile.eos() || _indexFile.err())
			break;

		logMessage("Reading index block '%s', size %d", tagToString(blocktype).c_str(), itemsize);

		if (!readIndexBlock(blocktype, itemsize)) {
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

bool ScummRandomizer::readIndexBlock(uint32 blocktype, uint32 itemsize) {
	switch (blocktype) {
	case MKTAG('D', 'O', 'B', 'J'):
		logMessage("Reading global objects");
		return readGlobalObjects();

	case MKTAG('D', 'R', 'O', 'O'):
	case MKTAG('D', 'I', 'R', 'R'):
		logMessage("Reading room list");
		return readResTypeList(rtRoom);

	case MKTAG('D', 'S', 'C', 'R'):
	case MKTAG('D', 'I', 'R', 'S'):
		logMessage("Reading script list");
		return readResTypeList(rtScript);

	case MKTAG('D', 'C', 'O', 'S'):
	case MKTAG('D', 'I', 'R', 'C'):
		logMessage("Reading costume list");
		return readResTypeList(rtCostume);

	case MKTAG('D', 'S', 'O', 'U'):
	case MKTAG('D', 'I', 'R', 'N'):
		logMessage("Reading sound list");
		return readResTypeList(rtSound);

	case MKTAG('M', 'A', 'X', 'S'):
		logMessage("Reading MAXS block");
		return readMAXS(itemsize);

	case MKTAG('R', 'N', 'A', 'M'):
		logMessage("Skipping room names");
		skipBytes(itemsize - 8);
		return true;

	default:
		logMessage("Skipping unknown block '%s'", tagToString(blocktype).c_str());
		skipBytes(itemsize - 8);
		return true;
	}
}

bool ScummRandomizer::readGlobalObjects() {
	int num = _indexFile.readUint16LE();
	_numGlobalObjects = num;

	logMessage("Reading %d global objects", num);

	_objects.resize(num);

	// Read owner table (adapted from ScummEngine::readGlobalObjects)
	for (int i = 0; i < num; i++) {
		byte ownerState = _indexFile.readByte();
		_objects[i].owner = ownerState & OF_OWNER_MASK;
		_objects[i].state = (ownerState & OF_STATE_MASK) >> OF_STATE_SHL;
	}

	// Read class data
	for (int i = 0; i < num; i++) {
		_objects[i].classData = _indexFile.readUint32LE();
	}

	dumpObjectInfo();
	return true;
}

bool ScummRandomizer::readResTypeList(int resType) {
	uint16 num = _indexFile.readUint16LE();

	logMessage("Reading %d resources of type %d", num, resType);

	// Just skip the resource entries for now
	// Each entry is typically: room_no (1 byte) + offset (4 bytes) = 5 bytes
	skipBytes(num * 5);

	return true;
}

bool ScummRandomizer::readMAXS(uint32 itemsize) {
	// MAXS block contains max counts for various resources
	// Just skip it for now
	skipBytes(itemsize - 8);
	return true;
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

	for (uint i = 0; i < _objects.size(); i++) {
		// Keep some objects with their original owners (player=1, room=0)
		// Randomize others between different rooms/characters
		if (_objects[i].owner != 0 && _objects[i].owner != 1) {
			// Randomly assign to rooms 0-15 or characters 1-3
			_objects[i].owner = _randomSource.getRandomNumber(15);
		}
	}

	logMessage("Object ownership randomized");
}

void ScummRandomizer::randomizeObjectStates() {
	logMessage("Randomizing object states...");

	for (uint i = 0; i < _objects.size(); i++) {
		// Object states are typically 0-15 (4 bits)
		// Be conservative and only modify some states
		if (_randomSource.getRandomNumber(2) == 0) { // 33% chance to modify
			_objects[i].state = _randomSource.getRandomNumber(15);
		}
	}

	logMessage("Object states randomized");
}

void ScummRandomizer::randomizeObjectClasses() {
	logMessage("Randomizing object classes...");

	for (uint i = 0; i < _objects.size(); i++) {
		// Class data is complex - only modify some bits carefully
		// Preserve important class flags but randomize others
		uint32 originalClass = _objects[i].classData;

		// Flip some random bits in the lower 16 bits (less critical)
		if (_randomSource.getRandomNumber(3) == 0) { // 25% chance to modify
			uint16 randomBits = _randomSource.getRandomNumber(0xFFFF);
			_objects[i].classData = (originalClass & 0xFFFF0000) | (randomBits & 0x0000FFFF);
		}
	}

	logMessage("Object classes randomized");
}

bool ScummRandomizer::writeIndexFile() {
	Common::Path outputIndexPath = _outputDir.appendComponent(_gameId + ".000");

	logMessage("Writing randomized index file: %s", outputIndexPath.toString().c_str());

	// Use ScummVM's DumpFile for writing
	Common::DumpFile outputFile;
	if (!outputFile.open(outputIndexPath)) {
		logMessage("ERROR: Cannot create output index file: %s", outputIndexPath.toString().c_str());
		return false;
	}

	// Re-read the original index file to copy structure
	Common::Path indexPath = _inputDir.appendComponent(_gameId + ".000");
	if (!_indexFile.open(indexPath)) {
		logMessage("ERROR: Cannot re-open index file: %s", indexPath.toString().c_str());
		return false;
	}

	// Copy the index file, but replace object data
	while (!_indexFile.eos() && !_indexFile.err()) {
		uint32 blocktype = _indexFile.readUint32BE();
		uint32 itemsize = _indexFile.readUint32BE();

		if (_indexFile.eos() || _indexFile.err())
			break;

		// Write block header
		outputFile.writeUint32BE(blocktype);
		outputFile.writeUint32BE(itemsize);

		if (blocktype == MKTAG('D', 'O', 'B', 'J')) {
			// Write our randomized object data
			writeGlobalObjects();
			outputFile.writeUint16LE(_objects.size());

			// Write owner/state table
			for (uint i = 0; i < _objects.size(); i++) {
				byte ownerState = (_objects[i].state << OF_STATE_SHL) | _objects[i].owner;
				outputFile.writeByte(ownerState);
			}

			// Write class data
			for (uint i = 0; i < _objects.size(); i++) {
				outputFile.writeUint32LE(_objects[i].classData);
			}
		} else {
			// Copy other blocks as-is
			Common::Array<byte> buffer;
			buffer.resize(itemsize - 8);
			_indexFile.read(buffer.data(), itemsize - 8);
			outputFile.write(buffer.data(), itemsize - 8);
		}
	}

	_indexFile.close();
	outputFile.close();

	logMessage("Index file written successfully");
	return true;
}

bool ScummRandomizer::writeGlobalObjects() {
	logMessage("Writing %d randomized global objects", _objects.size());
	return true;
}

bool ScummRandomizer::copyDataFile() {
	Common::Path inputDataPath = _inputDir.appendComponent(_gameId + ".001");
	Common::Path outputDataPath = _outputDir.appendComponent(_gameId + ".001");

	logMessage("Copying data file: %s -> %s",
			   inputDataPath.toString().c_str(), outputDataPath.toString().c_str());

	if (!_dataFile.open(inputDataPath)) {
		logMessage("ERROR: Cannot open input data file: %s", inputDataPath.toString().c_str());
		return false;
	}

	Common::DumpFile outputFile;
	if (!outputFile.open(outputDataPath)) {
		logMessage("ERROR: Cannot create output data file: %s", outputDataPath.toString().c_str());
		return false;
	}

	// Copy data file in chunks
	const int BUFFER_SIZE = 8192;
	byte buffer[BUFFER_SIZE];

	while (!_dataFile.eos()) {
		uint32 bytesRead = _dataFile.read(buffer, BUFFER_SIZE);
		if (bytesRead > 0) {
			outputFile.write(buffer, bytesRead);
		}
	}

	_dataFile.close();
	outputFile.close();

	logMessage("Data file copied successfully");
	return true;
}

void ScummRandomizer::skipBytes(uint32 count) {
	_indexFile.seek(count, SEEK_CUR);
}

Common::String ScummRandomizer::tagToString(uint32 tag) {
	char str[5];
	str[0] = (char)(tag >> 24);
	str[1] = (char)(tag >> 16);
	str[2] = (char)(tag >> 8);
	str[3] = (char)(tag);
	str[4] = 0;
	return Common::String(str);
}

void ScummRandomizer::dumpObjectInfo() {
	logMessage("=== Object Information ===");
	for (uint i = 0; i < _objects.size() && i < 20; i++) { // Show first 20 objects
		logMessage("Object %d: Owner=%d, State=%d, Class=0x%08x",
				   i, _objects[i].owner, _objects[i].state, _objects[i].classData);
	}
	if (_objects.size() > 20) {
		logMessage("... and %d more objects", _objects.size() - 20);
	}
}

void ScummRandomizer::logMessage(const char *format, ...) {
	va_list args;
	va_start(args, format);

	char buffer[1024];
	vsnprintf(buffer, sizeof(buffer), format, args);

	// Use ScummVM's debug output
	debug(1, "[SCUMM-Randomizer] %s", buffer);

	// Also print to stderr (avoiding stdout which is forbidden)
	warning("[SCUMM-Randomizer] %s", buffer);

	va_end(args);
}

} // End of namespace Scumm