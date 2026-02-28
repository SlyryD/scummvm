#include "scumm/scumm_v5.h"
#include "scumm/resource.h"
#include "scumm/object.h"
#include "common/array.h"
#include "common/random.h"
#include "common/file.h"
#include "common/endian.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "common/hashmap.h"

using namespace Scumm;

// ============================================================================
// Constants
// ============================================================================

static const byte XOR_KEY = 0x69;

// pickupObject opcode variants (SCUMM v5)
// 0x25: both params direct  (obj=word literal, room=byte literal)
// 0x65: param1 direct, param2 var (obj=word literal, room=var)
static const byte PICKUP_OPCODES[] = {0x25, 0x65};

// ============================================================================
// Struct: Catalog entry for an object
// ============================================================================

struct ObjCatalogEntry {
	uint16 objId;
	byte roomId;
	// Raw bytes of sub-blocks within the OBCD
	Common::Array<byte> cdhdBlock;   // Full CDHD block (tag+size+data)
	Common::Array<byte> verbBlock;   // Full VERB block
	Common::Array<byte> obnaBlock;   // Full OBNA block
	Common::Array<byte> fullObcd;    // Full OBCD block
	Common::String name;
	bool isPickuppable;

	// Parsed verb table: verb_id -> offset from VERB block start
	struct VerbEntry {
		byte verbId;
		uint16 offset;  // from VERB tag start
	};
	Common::Array<VerbEntry> verbTable;

	ObjCatalogEntry() : objId(0), roomId(0), isPickuppable(false) {}
};

// ============================================================================
// Helper: Parse verb table from a VERB block
// ============================================================================

static Common::Array<ObjCatalogEntry::VerbEntry> parseVerbTable(const byte *verbBlock) {
	Common::Array<ObjCatalogEntry::VerbEntry> entries;
	// Skip 8-byte header (tag+size), then read [verb_id(1) + offset(2 LE)]* terminated by 0x00
	const byte *p = verbBlock + 8;
	while (*p != 0x00) {
		ObjCatalogEntry::VerbEntry e;
		e.verbId = *p;
		e.offset = READ_LE_UINT16(p + 1);
		entries.push_back(e);
		p += 3;
	}
	return entries;
}

// ============================================================================
// Helper: Validate a candidate pickupObject opcode match.
//
//   0x25 (direct obj, direct room): 4 bytes total
//       byte[0]=op  byte[1..2]=obj_id(LE16)  byte[3]=room(uint8)
//       room byte must be 0 (current room) or in [1, numRooms].
//
//   0x65 (direct obj, variable room): 5 bytes total
//       byte[0]=op  byte[1..2]=obj_id(LE16)  byte[3..4]=var_idx(LE16)
//       var_idx must be < 0x4000 (0x2000 = array bit; anything ≥ 0x4000
//       is not a valid SCUMM v5 variable reference).
// ============================================================================

static bool validatePickupCandidate(const byte *verbBlock, uint16 pos,
                                     uint16 blockEnd, byte op, int numRooms) {
	if (op == 0x25) {
		// Need at least 4 bytes from pos
		if (pos + 3 >= blockEnd)
			return false;
		byte room = verbBlock[pos + 3];
		return (room == 0 || (room >= 1 && room <= numRooms));
	} else if (op == 0x65) {
		// Need at least 5 bytes from pos
		if (pos + 4 >= blockEnd)
			return false;
		uint16 varIdx = READ_LE_UINT16(verbBlock + pos + 3);
		return (varIdx < 0x4000);
	}
	return false;
}

// ============================================================================
// Helper: Check if a verb script region contains any pickupObject opcode
// ============================================================================

static bool verbScriptHasPickup(const byte *verbBlock, uint32 verbBlockSize,
                                 uint16 scriptOffset, uint16 nextOffset,
                                 int numRooms) {
	// scriptOffset and nextOffset are from VERB block start
	if (scriptOffset >= verbBlockSize) return false;
	uint16 end = (nextOffset > 0 && nextOffset <= verbBlockSize) ? nextOffset : verbBlockSize;

	for (uint16 i = scriptOffset; i + 2 < end; i++) {
		byte op = verbBlock[i];
		for (int k = 0; k < ARRAYSIZE(PICKUP_OPCODES); k++) {
			if (op == PICKUP_OPCODES[k] &&
			    validatePickupCandidate(verbBlock, i, end, op, numRooms))
				return true;
		}
	}
	return false;
}

// ============================================================================
// Helper: Extract all target object IDs from pickupObject opcodes in a
//         verb script region. For opcodes 0x25 and 0x65 the obj_id is the
//         LE uint16 immediately following the opcode byte.
// ============================================================================

static Common::Array<uint16> getPickupTargetObjIds(const byte *verbBlock, uint32 verbBlockSize,
                                                    uint16 scriptOffset, uint16 nextOffset,
                                                    int numRooms) {
	Common::Array<uint16> result;
	if (scriptOffset >= verbBlockSize) return result;
	uint16 end = (nextOffset > 0 && nextOffset <= verbBlockSize) ? nextOffset : verbBlockSize;

	for (uint16 i = scriptOffset; i + 2 < end; i++) {
		byte op = verbBlock[i];
		for (int k = 0; k < ARRAYSIZE(PICKUP_OPCODES); k++) {
			if (op == PICKUP_OPCODES[k] &&
			    validatePickupCandidate(verbBlock, i, end, op, numRooms)) {
				uint16 targetObjId = READ_LE_UINT16(verbBlock + i + 1);
				result.push_back(targetObjId);
				break;
			}
		}
	}
	return result;
}

// ============================================================================
// Helper: Get the byte range for a specific verb's script within a VERB block
// Returns (startOffset, endOffset) from VERB block start
// ============================================================================

static void getVerbScriptRange(const Common::Array<ObjCatalogEntry::VerbEntry> &table,
                                int idx, uint32 verbBlockSize,
                                uint16 &outStart, uint16 &outEnd) {
	outStart = table[idx].offset;

	// Find the smallest offset that is > outStart
	outEnd = (uint16)verbBlockSize;
	for (int j = 0; j < (int)table.size(); j++) {
		if (j == idx) continue;
		if (table[j].offset > outStart && table[j].offset < outEnd)
			outEnd = table[j].offset;
	}
}

// ============================================================================
// Helper: Build a merged VERB block
//
// Takes the slot's VERB block (for pickup scripts) and donor's VERB block
// (for all other scripts). Patches donor bytecode to replace donor obj_id
// with slot obj_id.
// ============================================================================

static Common::Array<byte> buildMergedVerbBlock(
	const ObjCatalogEntry &slot,
	const ObjCatalogEntry &donor) {

	const byte *slotVerb = slot.verbBlock.data();
	uint32 slotVerbSize = slot.verbBlock.size();
	const byte *donorVerb = donor.verbBlock.data();
	uint32 donorVerbSize = donor.verbBlock.size();

	// Parse both verb tables
	Common::Array<ObjCatalogEntry::VerbEntry> slotTable = parseVerbTable(slotVerb);
	Common::Array<ObjCatalogEntry::VerbEntry> donorTable = parseVerbTable(donorVerb);

	// Identify which verb entries in the slot have pickupObject(self)
	Common::Array<byte> pickupVerbIds;
	for (int i = 0; i < (int)slotTable.size(); i++) {
		uint16 sStart, sEnd;
		getVerbScriptRange(slotTable, i, slotVerbSize, sStart, sEnd);
		if (verbScriptHasPickup(slotVerb, slotVerbSize, sStart, sEnd, 127 /* numRooms */)) {
			pickupVerbIds.push_back(slotTable[i].verbId);
		}
	}

	// Build output: collect verb entries and their script bytecodes
	// Strategy: use donor verbs by default, but for pickup verb IDs, use slot's scripts
	struct MergedVerbScript {
		byte verbId;
		Common::Array<byte> scriptBytes;
	};
	Common::Array<MergedVerbScript> merged;

	// First, add all donor verbs (non-pickup), with obj_id patching
	for (int i = 0; i < (int)donorTable.size(); i++) {
		bool isPickup = false;
		for (int k = 0; k < (int)pickupVerbIds.size(); k++) {
			if (donorTable[i].verbId == pickupVerbIds[k]) {
				isPickup = true;
				break;
			}
		}
		if (isPickup) continue; // skip, will use slot's version

		uint16 sStart, sEnd;
		getVerbScriptRange(donorTable, i, donorVerbSize, sStart, sEnd);

		MergedVerbScript mv;
		mv.verbId = donorTable[i].verbId;
		mv.scriptBytes.resize(sEnd - sStart);
		memcpy(mv.scriptBytes.data(), donorVerb + sStart, sEnd - sStart);

		// Patch: replace donor obj_id with slot obj_id in all word-sized occurrences
		for (uint32 j = 0; j + 1 < mv.scriptBytes.size(); j++) {
			uint16 val = READ_LE_UINT16(mv.scriptBytes.data() + j);
			if (val == donor.objId) {
				WRITE_LE_UINT16(mv.scriptBytes.data() + j, slot.objId);
			}
		}

		merged.push_back(mv);
	}

	// Then, add slot's pickup verb scripts (unmodified)
	for (int k = 0; k < (int)pickupVerbIds.size(); k++) {
		byte pvId = pickupVerbIds[k];
		for (int i = 0; i < (int)slotTable.size(); i++) {
			if (slotTable[i].verbId == pvId) {
				uint16 sStart, sEnd;
				getVerbScriptRange(slotTable, i, slotVerbSize, sStart, sEnd);

				MergedVerbScript mv;
				mv.verbId = pvId;
				mv.scriptBytes.resize(sEnd - sStart);
				memcpy(mv.scriptBytes.data(), slotVerb + sStart, sEnd - sStart);
				merged.push_back(mv);
				break;
			}
		}
	}

	// Now assemble the VERB block
	// Calculate sizes
	uint32 tableSize = merged.size() * 3 + 1; // entries + terminator
	uint32 scriptDataSize = 0;
	for (int i = 0; i < (int)merged.size(); i++)
		scriptDataSize += merged[i].scriptBytes.size();

	uint32 verbDataSize = tableSize + scriptDataSize;
	uint32 totalVerbSize = 8 + verbDataSize; // tag(4) + size(4) + data

	Common::Array<byte> result(totalVerbSize);
	byte *out = result.data();

	// Tag
	WRITE_BE_UINT32(out, MKTAG('V', 'E', 'R', 'B'));
	WRITE_BE_UINT32(out + 4, totalVerbSize);

	// Verb table entries
	byte *tablePtr = out + 8;
	uint16 scriptOffset = 8 + tableSize; // from VERB block start
	for (int i = 0; i < (int)merged.size(); i++) {
		tablePtr[0] = merged[i].verbId;
		WRITE_LE_UINT16(tablePtr + 1, scriptOffset);
		tablePtr += 3;
		scriptOffset += merged[i].scriptBytes.size();
	}
	*tablePtr = 0x00; // terminator

	// Script data
	byte *scriptPtr = out + 8 + tableSize;
	for (int i = 0; i < (int)merged.size(); i++) {
		memcpy(scriptPtr, merged[i].scriptBytes.data(), merged[i].scriptBytes.size());
		scriptPtr += merged[i].scriptBytes.size();
	}

	return result;
}

// ============================================================================
// Helper: Build a new OBCD block from slot's CDHD + donor's OBNA + merged VERB
// ============================================================================

static Common::Array<byte> buildMergedObcd(
	const ObjCatalogEntry &slot,
	const ObjCatalogEntry &donor) {

	Common::Array<byte> mergedVerb = buildMergedVerbBlock(slot, donor);

	uint32 obcdDataSize = slot.cdhdBlock.size() + mergedVerb.size() + donor.obnaBlock.size();
	uint32 totalObcdSize = 8 + obcdDataSize;

	Common::Array<byte> result(totalObcdSize);
	byte *out = result.data();

	WRITE_BE_UINT32(out, MKTAG('O', 'B', 'C', 'D'));
	WRITE_BE_UINT32(out + 4, totalObcdSize);

	byte *p = out + 8;
	memcpy(p, slot.cdhdBlock.data(), slot.cdhdBlock.size());
	p += slot.cdhdBlock.size();
	memcpy(p, mergedVerb.data(), mergedVerb.size());
	p += mergedVerb.size();
	memcpy(p, donor.obnaBlock.data(), donor.obnaBlock.size());

	return result;
}

// ============================================================================
// Helper: Extract sub-block from a parent block (standard IFF: tag(4)+size(4)+data)
// ============================================================================

static Common::Array<byte> extractBlock(const byte *parent, uint32 parentSize, uint32 tag) {
	uint32 pos = 8;
	while (pos + 8 <= parentSize) {
		uint32 childTag = READ_BE_UINT32(parent + pos);
		uint32 childSize = READ_BE_UINT32(parent + pos + 4);
		if (childSize == 0 || childSize > parentSize - pos) break;
		if (childTag == tag) {
			Common::Array<byte> result(childSize);
			memcpy(result.data(), parent + pos, childSize);
			return result;
		}
		pos += childSize;
	}
	return Common::Array<byte>();
}

// ============================================================================
// Helper: Get object name from OBNA block
// ============================================================================

static Common::String getObjName(const Common::Array<byte> &obnaBlock) {
	if (obnaBlock.size() <= 8) return Common::String();
	return Common::String((const char *)(obnaBlock.data() + 8));
}

// ============================================================================
// Main implementation
// ============================================================================

// ============================================================================
// Helper: XOR-encrypt a buffer in-place
// ============================================================================

static void xorBuffer(byte *buf, uint32 size, byte key) {
	for (uint32 i = 0; i < size; i++)
		buf[i] ^= key;
}

// ============================================================================
// Main implementation
// ============================================================================

Common::Error ScummEngine_v5::randomizeGameFiles() {
	debug(0, "=== SCUMM V5 OBCD Swap Randomizer ===");

	// ------------------------------------------------------------------
	// Step 1: Catalog all objects across all rooms using the engine's
	//         resource loading infrastructure (openRoom, getResourceAddress,
	//         ResourceIterator, findResource, findResourceData).
	// ------------------------------------------------------------------

	Common::Array<ObjCatalogEntry> catalog;
	Common::HashMap<uint16, int> objToCatalogIdx;

	// Tracks which object IDs are targets of pickupObject opcodes.
	// Populated during the room scan, then applied to catalog entries afterward.
	Common::HashMap<uint16, bool> pickuppableObjIds;

	debug(0, "Scanning %d rooms for objects...", _numRooms);

	for (int roomId = 1; roomId < _numRooms; roomId++) {
		// Skip rooms that don't exist in the data files
		if (_res->_types[rtRoom][roomId]._roomoffs == RES_INVALID_OFFSET)
			continue;
		if (_res->_types[rtRoom][roomId]._roomoffs == 0 && roomId != 0)
			continue;

		// Load the room resource via the engine (handles file I/O, XOR decryption, etc.)
		const byte *roomPtr = getResourceAddress(rtRoom, roomId);
		if (!roomPtr)
			continue;

		// Get object count from RMHD
		const byte *rmhd = findResourceData(MKTAG('R', 'M', 'H', 'D'), roomPtr);
		if (!rmhd)
			continue;
		int numObjects = READ_LE_UINT16(&((const RoomHeader *)rmhd)->old.numObjects);
		if (numObjects == 0)
			continue;

		// Iterate OBCD blocks using ResourceIterator
		ResourceIterator obcds(roomPtr, false);
		for (int oi = 0; oi < numObjects; oi++) {
			const byte *obcdPtr = obcds.findNext(MKTAG('O', 'B', 'C', 'D'));
			if (!obcdPtr)
				break;

			uint32 obcdSize = READ_BE_UINT32(obcdPtr + 4);

			ObjCatalogEntry entry;
			entry.roomId = (byte)roomId;

			// Copy the full OBCD block
			entry.fullObcd.resize(obcdSize);
			memcpy(entry.fullObcd.data(), obcdPtr, obcdSize);

			// Extract sub-blocks (CDHD, VERB, OBNA)
			entry.cdhdBlock = extractBlock(obcdPtr, obcdSize, MKTAG('C', 'D', 'H', 'D'));
			entry.verbBlock = extractBlock(obcdPtr, obcdSize, MKTAG('V', 'E', 'R', 'B'));
			entry.obnaBlock = extractBlock(obcdPtr, obcdSize, MKTAG('O', 'B', 'N', 'A'));

			// Read obj_id from CDHD
			if (entry.cdhdBlock.size() >= 10) {
				entry.objId = READ_LE_UINT16(entry.cdhdBlock.data() + 8);
			}

			entry.name = getObjName(entry.obnaBlock);

			if (!entry.verbBlock.empty()) {
				entry.verbTable = parseVerbTable(entry.verbBlock.data());

				// Extract target object IDs from pickupObject opcodes;
				// the *target* is what's pickuppable, not this object.
				for (int vi = 0; vi < (int)entry.verbTable.size(); vi++) {
					uint16 sStart, sEnd;
					getVerbScriptRange(entry.verbTable, vi,
					                   entry.verbBlock.size(), sStart, sEnd);
					Common::Array<uint16> targets =
						getPickupTargetObjIds(entry.verbBlock.data(),
						                      entry.verbBlock.size(),
					                      sStart, sEnd,
					                      _numRooms);
					for (int ti = 0; ti < (int)targets.size(); ti++) {
						pickuppableObjIds[targets[ti]] = true;
					}
				}
			}

			int idx = catalog.size();
			catalog.push_back(entry);
			objToCatalogIdx[entry.objId] = idx;

			debug(1, "  Room %d: obj %d \"%s\"",
			      roomId, entry.objId, entry.name.c_str());
		}
	}

	// Mark catalog entries that are targets of a pickupObject opcode
	for (Common::HashMap<uint16, bool>::iterator it = pickuppableObjIds.begin();
	     it != pickuppableObjIds.end(); ++it) {
		uint16 targetId = it->_key;
		if (objToCatalogIdx.contains(targetId)) {
			catalog[objToCatalogIdx[targetId]].isPickuppable = true;
			debug(1, "  Marked obj %d as pickuppable", targetId);
		} else {
			debug(1, "  pickupObject target obj %d not found in any room OBCD", targetId);
		}
	}

	// Collect pickuppable indices
	Common::Array<int> pickuppableIndices;
	for (int i = 0; i < (int)catalog.size(); i++) {
		if (catalog[i].isPickuppable)
			pickuppableIndices.push_back(i);
	}

	debug(0, "Total objects: %d, pick-uppable: %d", catalog.size(), pickuppableIndices.size());
	for (int i = 0; i < (int)pickuppableIndices.size(); i++) {
		const ObjCatalogEntry &e = catalog[pickuppableIndices[i]];
		debug(0, "  Pickuppable: obj %d in room %d: \"%s\"", e.objId, e.roomId, e.name.c_str());
	}

	if (pickuppableIndices.size() < 2) {
		warning("randomizer: Not enough pickuppable objects to shuffle");
		return Common::kNoError;
	}

	return Common::kNoError;

#if 0
	// ------------------------------------------------------------------
	// Step 2: Fisher-Yates shuffle of pickuppable objects
	// ------------------------------------------------------------------

	Common::RandomSource rng("scummv5randomizer");

	Common::Array<int> shuffled(pickuppableIndices.size());
	for (int i = 0; i < (int)shuffled.size(); i++)
		shuffled[i] = i;

	for (int i = (int)shuffled.size() - 1; i > 0; i--) {
		int j = rng.getRandomNumber(i);
		SWAP(shuffled[i], shuffled[j]);
	}

	debug(0, "Shuffle mapping:");
	for (int i = 0; i < (int)shuffled.size(); i++) {
		int slotIdx = pickuppableIndices[i];
		int donorIdx = pickuppableIndices[shuffled[i]];
		debug(0, "  Slot obj %d (\"%s\") <- Donor obj %d (\"%s\")",
		      catalog[slotIdx].objId, catalog[slotIdx].name.c_str(),
		      catalog[donorIdx].objId, catalog[donorIdx].name.c_str());
	}

	// ------------------------------------------------------------------
	// Step 3: Build merged OBCDs for each swapped pair
	// ------------------------------------------------------------------

	Common::HashMap<uint16, Common::Array<byte>> newObcds;

	for (int i = 0; i < (int)shuffled.size(); i++) {
		int slotIdx = pickuppableIndices[i];
		int donorIdx = pickuppableIndices[shuffled[i]];

		if (slotIdx == donorIdx) continue;

		const ObjCatalogEntry &slot = catalog[slotIdx];
		const ObjCatalogEntry &donor = catalog[donorIdx];

		Common::Array<byte> merged = buildMergedObcd(slot, donor);
		newObcds[slot.objId] = merged;

		debug(0, "  Built merged OBCD for slot obj %d (size %u -> %u)",
		      slot.objId, slot.fullObcd.size(), merged.size());
	}

	// ------------------------------------------------------------------
	// Step 4: Rebuild each modified room by patching the in-memory
	//         room resource data loaded by the engine.
	// ------------------------------------------------------------------

	Common::HashMap<byte, bool> modifiedRooms;
	for (Common::HashMap<uint16, Common::Array<byte>>::iterator it = newObcds.begin();
	     it != newObcds.end(); ++it) {
		uint16 objId = it->_key;
		if (objToCatalogIdx.contains(objId)) {
			modifiedRooms[catalog[objToCatalogIdx[objId]].roomId] = true;
		}
	}

	// roomNewBlocks: roomId -> new ROOM block bytes
	Common::HashMap<byte, Common::Array<byte>> roomNewBlocks;

	for (int roomId = 1; roomId < _numRooms; roomId++) {
		if (!modifiedRooms.contains((byte)roomId))
			continue;

		const byte *roomPtr = getResourceAddress(rtRoom, roomId);
		if (!roomPtr)
			continue;

		uint32 roomSize = READ_BE_UINT32(roomPtr + 4);

		// Walk children of ROOM, copy unchanged blocks, substitute modified OBCDs
		Common::Array<byte> newRoomData;
		uint32 pos = 8;

		while (pos + 8 <= roomSize) {
			uint32 childTag = READ_BE_UINT32(roomPtr + pos);
			uint32 childSize = READ_BE_UINT32(roomPtr + pos + 4);
			if (childSize == 0 || childSize > roomSize - pos) break;

			if (childTag == MKTAG('O', 'B', 'C', 'D')) {
				// Check if this OBCD should be replaced
				uint16 objId = 0;
				Common::Array<byte> cdhd = extractBlock(roomPtr + pos, childSize,
				                                         MKTAG('C', 'D', 'H', 'D'));
				if (cdhd.size() >= 10) {
					objId = READ_LE_UINT16(cdhd.data() + 8);
				}

				if (newObcds.contains(objId)) {
					const Common::Array<byte> &merged = newObcds[objId];
					for (uint32 b = 0; b < merged.size(); b++)
						newRoomData.push_back(merged[b]);
					debug(1, "  Room %d: replaced OBCD for obj %d (old size %u, new size %u)",
					      roomId, objId, childSize, merged.size());
				} else {
					for (uint32 b = 0; b < childSize; b++)
						newRoomData.push_back(roomPtr[pos + b]);
				}
			} else {
				for (uint32 b = 0; b < childSize; b++)
					newRoomData.push_back(roomPtr[pos + b]);
			}

			pos += childSize;
		}

		// Build ROOM block with updated size
		uint32 newRoomSize = 8 + newRoomData.size();
		Common::Array<byte> newRoom(newRoomSize);
		WRITE_BE_UINT32(newRoom.data(), MKTAG('R', 'O', 'O', 'M'));
		WRITE_BE_UINT32(newRoom.data() + 4, newRoomSize);
		memcpy(newRoom.data() + 8, newRoomData.data(), newRoomData.size());

		roomNewBlocks[(byte)roomId] = newRoom;
		debug(0, "  Rebuilt room %d: %u -> %u bytes", roomId, roomSize, newRoomSize);
	}

	// ------------------------------------------------------------------
	// Step 5: Reconstruct the .001 data file
	//
	// Read the original file structure using openRoom / _res metadata,
	// then write new LECF ( LOFF, LFLF* ) with patched rooms.
	// ------------------------------------------------------------------

	// Gather disk-level room info from the resource manager
	struct RoomFileInfo {
		byte roomId;
		uint32 origRoomOffs;  // offset of ROOM within the original file
	};
	Common::Array<RoomFileInfo> roomFileInfos;

	for (int roomId = 1; roomId < _numRooms; roomId++) {
		if (_res->_types[rtRoom][roomId]._roomoffs == RES_INVALID_OFFSET)
			continue;
		if (_res->_types[rtRoom][roomId]._roomoffs == 0 && roomId != 0)
			continue;

		RoomFileInfo rfi;
		rfi.roomId = (byte)roomId;
		rfi.origRoomOffs = _res->_types[rtRoom][roomId]._roomoffs;
		roomFileInfos.push_back(rfi);
	}

	// Read the original .001 file (we need the raw LFLF structure including
	// extra blocks like SCRP, SOUN, COST, CHAR that sit alongside ROOM).
	// Use the engine's openRoom / file handle to read it properly.
	Common::Path dataFilename(generateFilename(1));

	// Read the original raw file ourselves for the LFLF structure
	// (the engine's loadResource only loads individual resources, not the
	// full LFLF containers we need for reconstruction).
	Common::File rawFile;
	if (!rawFile.open(dataFilename)) {
		warning("randomizer: Cannot open data file %s", dataFilename.toString().c_str());
		return Common::kPathNotFile;
	}

	uint32 rawSize = rawFile.size();
	Common::Array<byte> rawData(rawSize);
	rawFile.read(rawData.data(), rawSize);
	rawFile.close();

	// XOR-decrypt the raw file (v5 uses 0x69 encryption)
	xorBuffer(rawData.data(), rawData.size(), XOR_KEY);

	const byte *d001 = rawData.data();
	uint32 d001Size = rawData.size();

	// Verify LECF header
	if (READ_BE_UINT32(d001) != MKTAG('L', 'E', 'C', 'F')) {
		warning("randomizer: Data file does not start with LECF");
		return Common::kUnknownError;
	}

	// Parse LOFF block to get room offsets within the file
	if (READ_BE_UINT32(d001 + 8) != MKTAG('L', 'O', 'F', 'F')) {
		warning("randomizer: LOFF not found at expected offset");
		return Common::kUnknownError;
	}

	byte numFileRooms = d001[16];
	struct RoomOffset {
		byte roomId;
		uint32 offset;
	};
	Common::Array<RoomOffset> roomOffsets;

	uint32 loffPos = 17;
	for (int i = 0; i < numFileRooms; i++) {
		RoomOffset ro;
		ro.roomId = d001[loffPos];
		ro.offset = READ_LE_UINT32(d001 + loffPos + 1);
		roomOffsets.push_back(ro);
		loffPos += 5;
	}

	// Build new LOFF + LFLF blocks
	uint32 newLoffSize = 8 + 1 + numFileRooms * 5;
	uint32 currentOffset = 8 + newLoffSize;

	struct NewRoomInfo {
		byte roomId;
		uint32 lflfOffset;
		uint32 roomOffset;
		Common::Array<byte> roomBlock;
		Common::Array<byte> extraBlocks;
	};
	Common::Array<NewRoomInfo> newRoomInfos;

	for (int ri = 0; ri < (int)roomOffsets.size(); ri++) {
		byte rmId = roomOffsets[ri].roomId;
		uint32 origRoomOff = roomOffsets[ri].offset;

		NewRoomInfo info;
		info.roomId = rmId;

		// Use modified room block if available, else copy original from raw data
		if (roomNewBlocks.contains(rmId)) {
			info.roomBlock = roomNewBlocks[rmId];
		} else {
			if (origRoomOff + 8 <= d001Size &&
			    READ_BE_UINT32(d001 + origRoomOff) == MKTAG('R', 'O', 'O', 'M')) {
				uint32 origSize = READ_BE_UINT32(d001 + origRoomOff + 4);
				info.roomBlock.resize(origSize);
				memcpy(info.roomBlock.data(), d001 + origRoomOff, origSize);
			}
		}

		// Collect extra blocks (SCRP, SOUN, COST, CHAR, etc.) after ROOM in the LFLF
		uint32 lflfOff = origRoomOff - 8;
		if (lflfOff < d001Size &&
		    READ_BE_UINT32(d001 + lflfOff) == MKTAG('L', 'F', 'L', 'F')) {
			uint32 lflfSize = READ_BE_UINT32(d001 + lflfOff + 4);
			uint32 lflfEnd = lflfOff + lflfSize;
			uint32 origRoomSize = READ_BE_UINT32(d001 + origRoomOff + 4);
			uint32 extraStart = origRoomOff + origRoomSize;

			if (extraStart < lflfEnd) {
				uint32 extraSize = lflfEnd - extraStart;
				info.extraBlocks.resize(extraSize);
				memcpy(info.extraBlocks.data(), d001 + extraStart, extraSize);
			}
		}

		info.lflfOffset = currentOffset;
		info.roomOffset = currentOffset + 8;

		uint32 lflfContentSize = info.roomBlock.size() + info.extraBlocks.size();
		currentOffset += 8 + lflfContentSize;
		newRoomInfos.push_back(info);
	}

	uint32 lecfTotalSize = currentOffset;

	debug(0, "Writing new .001 file (%u bytes)", lecfTotalSize);

	Common::Array<byte> output(lecfTotalSize);
	byte *out = output.data();
	memset(out, 0, lecfTotalSize);

	// LECF header
	WRITE_BE_UINT32(out, MKTAG('L', 'E', 'C', 'F'));
	WRITE_BE_UINT32(out + 4, lecfTotalSize);

	// LOFF block
	byte *loffPtr = out + 8;
	WRITE_BE_UINT32(loffPtr, MKTAG('L', 'O', 'F', 'F'));
	WRITE_BE_UINT32(loffPtr + 4, newLoffSize);
	loffPtr[8] = numFileRooms;

	byte *loffEntries = loffPtr + 9;
	for (int i = 0; i < (int)newRoomInfos.size(); i++) {
		loffEntries[0] = newRoomInfos[i].roomId;
		WRITE_LE_UINT32(loffEntries + 1, newRoomInfos[i].roomOffset);
		loffEntries += 5;
	}

	// LFLF blocks
	for (int i = 0; i < (int)newRoomInfos.size(); i++) {
		const NewRoomInfo &info = newRoomInfos[i];
		byte *lflfPtr = out + info.lflfOffset;

		uint32 lflfContentSize = info.roomBlock.size() + info.extraBlocks.size();
		uint32 lflfTotalSize = 8 + lflfContentSize;

		WRITE_BE_UINT32(lflfPtr, MKTAG('L', 'F', 'L', 'F'));
		WRITE_BE_UINT32(lflfPtr + 4, lflfTotalSize);

		memcpy(lflfPtr + 8, info.roomBlock.data(), info.roomBlock.size());
		if (!info.extraBlocks.empty()) {
			memcpy(lflfPtr + 8 + info.roomBlock.size(),
			       info.extraBlocks.data(), info.extraBlocks.size());
		}
	}

	// XOR-encrypt the output
	xorBuffer(output.data(), output.size(), XOR_KEY);

	// Write to file
	Common::DumpFile outFile;
	if (!outFile.open(Common::Path("randomizer/output/MONKEY2/MONKEY2.001"), true)) {
		warning("randomizer: Cannot open output .001 file");
		return Common::kWritingFailed;
	}
	outFile.write(output.data(), output.size());
	outFile.close();
	debug(0, "Wrote .001 (%u bytes)", output.size());

	// ------------------------------------------------------------------
	// Step 6: Write updated .000 index file
	//
	// Read the original index, patch DROO room offsets to reflect
	// the new LFLF layout.
	// ------------------------------------------------------------------

	// Read the original .000 index file
	Common::Path indexFilename(generateFilename(0));
	Common::File idxRawFile;
	if (!idxRawFile.open(indexFilename)) {
		warning("randomizer: Cannot open index file %s", indexFilename.toString().c_str());
		return Common::kPathNotFile;
	}

	uint32 idxSize = idxRawFile.size();
	Common::Array<byte> newIndex(idxSize);
	idxRawFile.read(newIndex.data(), idxSize);
	idxRawFile.close();

	// XOR-decrypt the index
	xorBuffer(newIndex.data(), newIndex.size(), XOR_KEY);

	// Find and patch the DROO block with new room offsets
	uint32 idxPos = 0;
	while (idxPos + 8 < newIndex.size()) {
		uint32 blockTag = READ_BE_UINT32(newIndex.data() + idxPos);
		uint32 blockSize = READ_BE_UINT32(newIndex.data() + idxPos + 4);
		if (blockSize == 0) break;

		if (blockTag == MKTAG('D', 'R', 'O', 'O')) {
			uint16 num = READ_LE_UINT16(newIndex.data() + idxPos + 8);
			uint32 roomOffsStart = idxPos + 8 + 2 + num;

			for (int ri = 0; ri < (int)newRoomInfos.size(); ri++) {
				byte rmId = newRoomInfos[ri].roomId;
				if (rmId < num) {
					WRITE_LE_UINT32(newIndex.data() + roomOffsStart + rmId * 4,
					                newRoomInfos[ri].roomOffset);
				}
			}

			debug(0, "Patched DROO with %d room offsets", newRoomInfos.size());
			break;
		}

		idxPos += blockSize;
	}

	// XOR-encrypt and write
	xorBuffer(newIndex.data(), newIndex.size(), XOR_KEY);

	Common::DumpFile idxFile;
	if (!idxFile.open(Common::Path("randomizer/output/MONKEY2/MONKEY2.000"), true)) {
		warning("randomizer: Cannot open output .000 file");
		return Common::kWritingFailed;
	}
	idxFile.write(newIndex.data(), newIndex.size());
	idxFile.close();
	debug(0, "Wrote .000 (%u bytes)", newIndex.size());

	debug(0, "=== Randomization complete! ===");
	return Common::kNoError;
#endif
}
