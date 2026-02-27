#include "scumm/scumm_v5.h"
#include "scumm/resource.h"
#include "scumm/object.h"
#include "common/array.h"
#include "common/random.h"
#include "common/file.h"
#include "common/memstream.h"
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
// 0xA5: param1 var, param2 direct (obj=var, room=byte literal)
// 0xE5: both params var     (obj=var, room=var)
static const byte PICKUP_DIRECT_OPCODES[] = {0x25, 0x65};

// SCUMM v5 verb ID for "Pick up"
static const byte VERB_PICKUP = 0x09;

// ============================================================================
// Helper: Read a raw file from disk (no XOR decryption - that's in ScummFile)
// ============================================================================

static Common::Array<byte> readRawFile(const Common::Path &path) {
	Common::File f;
	if (!f.open(path)) {
		warning("randomizer: Cannot open %s", path.toString().c_str());
		return Common::Array<byte>();
	}
	uint32 sz = f.size();
	Common::Array<byte> buf(sz);
	f.read(buf.data(), sz);
	f.close();
	return buf;
}

// ============================================================================
// Helper: XOR-decrypt/encrypt a buffer in-place
// ============================================================================

static void xorBuffer(byte *buf, uint32 size, byte key) {
	for (uint32 i = 0; i < size; i++)
		buf[i] ^= key;
}

// ============================================================================
// Helper: Read a BE uint32 tag from a buffer
// ============================================================================

static uint32 readTag(const byte *p) {
	return READ_BE_UINT32(p);
}

static uint32 readSize(const byte *p) {
	return READ_BE_UINT32(p + 4);
}

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
// Helper: Check if a verb script region contains pickupObject(self)
// ============================================================================

static bool verbScriptHasPickupSelf(const byte *verbBlock, uint32 verbBlockSize,
                                     uint16 scriptOffset, uint16 nextOffset,
                                     uint16 objId) {
	// scriptOffset and nextOffset are from VERB block start
	if (scriptOffset >= verbBlockSize) return false;
	uint16 end = (nextOffset > 0 && nextOffset <= verbBlockSize) ? nextOffset : verbBlockSize;

	for (uint16 i = scriptOffset; i + 2 < end; i++) {
		byte op = verbBlock[i];
		if (op == 0x25 || op == 0x65) {
			uint16 pid = READ_LE_UINT16(verbBlock + i + 1);
			if (pid == objId)
				return true;
		}
	}
	return false;
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
		if (verbScriptHasPickupSelf(slotVerb, slotVerbSize, sStart, sEnd, slot.objId)) {
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
// Helper: Extract sub-block from a parent block
// ============================================================================

static Common::Array<byte> extractBlock(const byte *parent, uint32 tag) {
	uint32 parentSize = readSize(parent);
	uint32 pos = 8;
	while (pos < parentSize) {
		uint32 childTag = readTag(parent + pos);
		uint32 childSize = readSize(parent + pos);
		if (childSize == 0) break;
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

Common::Error ScummEngine_v5::randomizeGameFiles() {
	debug(0, "=== SCUMM V5 OBCD Swap Randomizer ===");

	// ------------------------------------------------------------------
	// Step 0: Read the raw .001 and .000 files
	// ------------------------------------------------------------------

	Common::Path inputDir("randomizer/input/MONKEY2/");
	Common::Path outputDir("randomizer/output/MONKEY2/");

	Common::Array<byte> data001 = readRawFile(Common::Path("randomizer/input/MONKEY2/MONKEY2.001"));
	Common::Array<byte> data000 = readRawFile(Common::Path("randomizer/input/MONKEY2/MONKEY2.000"));

	if (data001.empty() || data000.empty()) {
		warning("randomizer: Could not read input files");
		return Common::kPathNotFile;
	}

	// XOR-decrypt both files
	xorBuffer(data001.data(), data001.size(), XOR_KEY);
	xorBuffer(data000.data(), data000.size(), XOR_KEY);

	debug(0, "Read .001 (%u bytes) and .000 (%u bytes)", data001.size(), data000.size());

	const byte *d001 = data001.data();
	uint32 d001Size = data001.size();

	// ------------------------------------------------------------------
	// Step 1: Parse LOFF to get room offsets
	// ------------------------------------------------------------------

	// LECF at offset 0, LOFF at offset 8
	if (readTag(d001) != MKTAG('L', 'E', 'C', 'F')) {
		warning("randomizer: .001 does not start with LECF");
		return Common::kUnknownError;
	}
	if (readTag(d001 + 8) != MKTAG('L', 'O', 'F', 'F')) {
		warning("randomizer: LOFF not found at offset 8");
		return Common::kUnknownError;
	}

	byte numRooms = d001[16];
	debug(0, "LOFF: %d rooms", numRooms);

	struct RoomOffset {
		byte roomId;
		uint32 offset; // offset of ROOM block within .001
	};
	Common::Array<RoomOffset> roomOffsets;

	uint32 loffPos = 17;
	for (int i = 0; i < numRooms; i++) {
		RoomOffset ro;
		ro.roomId = d001[loffPos];
		ro.offset = READ_LE_UINT32(d001 + loffPos + 1);
		roomOffsets.push_back(ro);
		loffPos += 5;
		debug(1, "  Room %d at offset %u", ro.roomId, ro.offset);
	}

	// ------------------------------------------------------------------
	// Step 2: Catalog all objects, identify pick-uppable ones
	// ------------------------------------------------------------------

	Common::Array<ObjCatalogEntry> catalog;
	// Map from obj_id to catalog index for quick lookup
	Common::HashMap<uint16, int> objToCatalogIdx;

	for (int ri = 0; ri < (int)roomOffsets.size(); ri++) {
		uint32 roomOff = roomOffsets[ri].offset;
		byte roomId = roomOffsets[ri].roomId;

		if (roomOff + 8 > d001Size) continue;
		if (readTag(d001 + roomOff) != MKTAG('R', 'O', 'O', 'M')) continue;

		uint32 roomSize = readSize(d001 + roomOff);

		// Walk children of ROOM to find OBCD blocks
		uint32 pos = roomOff + 8;
		uint32 roomEnd = roomOff + roomSize;

		while (pos + 8 <= roomEnd) {
			uint32 childTag = readTag(d001 + pos);
			uint32 childSize = readSize(d001 + pos);
			if (childSize == 0 || childSize > roomEnd - pos) break;

			if (childTag == MKTAG('O', 'B', 'C', 'D')) {
				const byte *obcdPtr = d001 + pos;

				ObjCatalogEntry entry;
				entry.roomId = roomId;
				entry.fullObcd.resize(childSize);
				memcpy(entry.fullObcd.data(), obcdPtr, childSize);

				// Extract sub-blocks
				entry.cdhdBlock = extractBlock(obcdPtr, MKTAG('C', 'D', 'H', 'D'));
				entry.verbBlock = extractBlock(obcdPtr, MKTAG('V', 'E', 'R', 'B'));
				entry.obnaBlock = extractBlock(obcdPtr, MKTAG('O', 'B', 'N', 'A'));

				if (entry.cdhdBlock.size() >= 10) {
					entry.objId = READ_LE_UINT16(entry.cdhdBlock.data() + 8);
				}

				entry.name = getObjName(entry.obnaBlock);

				// Check if pick-uppable: has verb 0x09 with pickupObject(self)
				if (!entry.verbBlock.empty()) {
					entry.verbTable = parseVerbTable(entry.verbBlock.data());

					for (int vi = 0; vi < (int)entry.verbTable.size(); vi++) {
						if (entry.verbTable[vi].verbId == VERB_PICKUP) {
							uint16 sStart, sEnd;
							getVerbScriptRange(entry.verbTable, vi,
							                   entry.verbBlock.size(), sStart, sEnd);
							if (verbScriptHasPickupSelf(entry.verbBlock.data(),
							                             entry.verbBlock.size(),
							                             sStart, sEnd, entry.objId)) {
								entry.isPickuppable = true;
							}
							break;
						}
					}
				}

				int idx = catalog.size();
				catalog.push_back(entry);
				objToCatalogIdx[entry.objId] = idx;

				debug(1, "  Room %d: obj %d \"%s\" %s",
				      roomId, entry.objId, entry.name.c_str(),
				      entry.isPickuppable ? "[PICKUPPABLE]" : "");
			}

			pos += childSize;
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

	// ------------------------------------------------------------------
	// Step 3: Fisher-Yates shuffle of pickuppable objects
	// ------------------------------------------------------------------

	Common::RandomSource rng("scummv5randomizer");

	// Create a permutation of the pickuppable objects
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
	// Step 4: Build merged OBCDs for each swapped pair
	// ------------------------------------------------------------------

	// Map from obj_id to new OBCD bytes (only for objects that changed)
	Common::HashMap<uint16, Common::Array<byte>> newObcds;

	for (int i = 0; i < (int)shuffled.size(); i++) {
		int slotIdx = pickuppableIndices[i];
		int donorIdx = pickuppableIndices[shuffled[i]];

		if (slotIdx == donorIdx) continue; // No swap needed

		const ObjCatalogEntry &slot = catalog[slotIdx];
		const ObjCatalogEntry &donor = catalog[donorIdx];

		Common::Array<byte> merged = buildMergedObcd(slot, donor);
		newObcds[slot.objId] = merged;

		debug(0, "  Built merged OBCD for slot obj %d (size %u -> %u)",
		      slot.objId, slot.fullObcd.size(), merged.size());
	}

	// ------------------------------------------------------------------
	// Step 5: Rebuild each modified room in the .001 file
	// ------------------------------------------------------------------

	// We need to track which rooms have modified objects
	Common::HashMap<byte, bool> modifiedRooms;
	for (Common::HashMap<uint16, Common::Array<byte>>::iterator it = newObcds.begin();
	     it != newObcds.end(); ++it) {
		uint16 objId = it->_key;
		if (objToCatalogIdx.contains(objId)) {
			modifiedRooms[catalog[objToCatalogIdx[objId]].roomId] = true;
		}
	}

	// For each room, build the new ROOM block.
	// roomNewBlocks: roomId -> new ROOM bytes
	Common::HashMap<byte, Common::Array<byte>> roomNewBlocks;

	for (int ri = 0; ri < (int)roomOffsets.size(); ri++) {
		byte roomId = roomOffsets[ri].roomId;
		uint32 roomOff = roomOffsets[ri].offset;

		if (!modifiedRooms.contains(roomId)) continue;

		if (readTag(d001 + roomOff) != MKTAG('R', 'O', 'O', 'M')) continue;
		uint32 roomSize = readSize(d001 + roomOff);

		// Walk children, copy unchanged, substitute modified OBCDs
		Common::Array<byte> newRoomData;

		uint32 pos = roomOff + 8;
		uint32 roomEnd = roomOff + roomSize;

		while (pos + 8 <= roomEnd) {
			uint32 childTag = readTag(d001 + pos);
			uint32 childSize = readSize(d001 + pos);
			if (childSize == 0 || childSize > roomEnd - pos) break;

			if (childTag == MKTAG('O', 'B', 'C', 'D')) {
				// Check if this OBCD has been modified
				uint16 objId = 0;
				Common::Array<byte> cdhd = extractBlock(d001 + pos, MKTAG('C', 'D', 'H', 'D'));
				if (cdhd.size() >= 10) {
					objId = READ_LE_UINT16(cdhd.data() + 8);
				}

				if (newObcds.contains(objId)) {
					// Use the merged OBCD
					const Common::Array<byte> &merged = newObcds[objId];
					for (uint32 b = 0; b < merged.size(); b++)
						newRoomData.push_back(merged[b]);
					debug(1, "  Room %d: replaced OBCD for obj %d (old size %u, new size %u)",
					      roomId, objId, childSize, merged.size());
				} else {
					// Copy original
					for (uint32 b = 0; b < childSize; b++)
						newRoomData.push_back(d001[pos + b]);
				}
			} else {
				// Copy unchanged child block
				for (uint32 b = 0; b < childSize; b++)
					newRoomData.push_back(d001[pos + b]);
			}

			pos += childSize;
		}

		// Build the complete ROOM block with updated size
		uint32 newRoomSize = 8 + newRoomData.size();
		Common::Array<byte> newRoom(newRoomSize);
		WRITE_BE_UINT32(newRoom.data(), MKTAG('R', 'O', 'O', 'M'));
		WRITE_BE_UINT32(newRoom.data() + 4, newRoomSize);
		memcpy(newRoom.data() + 8, newRoomData.data(), newRoomData.size());

		roomNewBlocks[roomId] = newRoom;
		debug(0, "  Rebuilt room %d: %u -> %u bytes", roomId, roomSize, newRoomSize);
	}

	// ------------------------------------------------------------------
	// Step 6: Write new .001 file
	// ------------------------------------------------------------------

	// Structure: LECF ( LOFF, LFLF*, ... )
	// Each LFLF wraps a ROOM block (and potentially SCRP, SOUN, COST, CHAR blocks)

	// First, compute the new LOFF and LFLF blocks
	// LOFF size = 8 (header) + 1 (numRooms) + numRooms * 5
	uint32 newLoffSize = 8 + 1 + numRooms * 5;

	// Calculate where each LFLF will go
	// LECF header (8) + LOFF block
	uint32 currentOffset = 8 + newLoffSize;

	struct NewRoomInfo {
		byte roomId;
		uint32 lflfOffset;   // from file start
		uint32 roomOffset;   // from file start (LFLF + 8)
		Common::Array<byte> roomBlock;
		Common::Array<byte> extraBlocks; // SCRP, SOUN, COST, CHAR etc. after ROOM
	};
	Common::Array<NewRoomInfo> newRoomInfos;

	for (int ri = 0; ri < (int)roomOffsets.size(); ri++) {
		byte roomId = roomOffsets[ri].roomId;
		uint32 origRoomOff = roomOffsets[ri].offset;

		NewRoomInfo info;
		info.roomId = roomId;

		// Get the room block (new or original)
		if (roomNewBlocks.contains(roomId)) {
			info.roomBlock = roomNewBlocks[roomId];
		} else {
			// Copy original ROOM block
			if (origRoomOff + 8 <= d001Size && readTag(d001 + origRoomOff) == MKTAG('R', 'O', 'O', 'M')) {
				uint32 origSize = readSize(d001 + origRoomOff);
				info.roomBlock.resize(origSize);
				memcpy(info.roomBlock.data(), d001 + origRoomOff, origSize);
			}
		}

		// Collect extra blocks after ROOM in the same LFLF
		// LFLF starts at origRoomOff - 8
		uint32 lflfOff = origRoomOff - 8;
		if (lflfOff < d001Size && readTag(d001 + lflfOff) == MKTAG('L', 'F', 'L', 'F')) {
			uint32 lflfSize = readSize(d001 + lflfOff);
			uint32 lflfEnd = lflfOff + lflfSize;
			uint32 origRoomSize = readSize(d001 + origRoomOff);
			uint32 extraStart = origRoomOff + origRoomSize;

			if (extraStart < lflfEnd) {
				uint32 extraSize = lflfEnd - extraStart;
				info.extraBlocks.resize(extraSize);
				memcpy(info.extraBlocks.data(), d001 + extraStart, extraSize);
			}
		}

		// Calculate LFLF offset (from file start, not from LECF start)
		info.lflfOffset = currentOffset;
		info.roomOffset = currentOffset + 8; // LFLF header is 8 bytes

		// LFLF size = 8 (LFLF header) + ROOM block + extra blocks
		uint32 lflfContentSize = info.roomBlock.size() + info.extraBlocks.size();
		uint32 lflfTotalSize = 8 + lflfContentSize;

		currentOffset += lflfTotalSize;
		newRoomInfos.push_back(info);
	}

	uint32 lecfTotalSize = currentOffset; // Total file size = LECF size

	// Now write the file
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
	loffPtr[8] = numRooms;

	byte *loffEntries = loffPtr + 9;
	for (int i = 0; i < (int)newRoomInfos.size(); i++) {
		loffEntries[0] = newRoomInfos[i].roomId;
		WRITE_LE_UINT32(loffEntries + 1, newRoomInfos[i].roomOffset); // LOFF points to ROOM
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
	// Step 7: Write updated .000 index file
	// ------------------------------------------------------------------

	// Copy the .000 file, then patch the DROO block's room offsets
	Common::Array<byte> newIndex(data000); // already decrypted

	// Find DROO block in the index
	uint32 idxPos = 0;
	while (idxPos + 8 < newIndex.size()) {
		uint32 blockTag = readTag(newIndex.data() + idxPos);
		uint32 blockSize = readSize(newIndex.data() + idxPos);
		if (blockSize == 0) break;

		if (blockTag == MKTAG('D', 'R', 'O', 'O')) {
			// DROO format: tag(4) + size(4) + num(2 LE) + roomno[num](1 each) + roomoffs[num](4 LE each)
			uint16 num = READ_LE_UINT16(newIndex.data() + idxPos + 8);
			uint32 roomOffsStart = idxPos + 8 + 2 + num; // after num + roomno array

			// Update room offsets
			for (int ri = 0; ri < (int)newRoomInfos.size(); ri++) {
				byte roomId = newRoomInfos[ri].roomId;
				if (roomId < num) {
					WRITE_LE_UINT32(newIndex.data() + roomOffsStart + roomId * 4,
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
}
