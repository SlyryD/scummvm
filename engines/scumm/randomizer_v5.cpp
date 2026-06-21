#include "scumm/randomizer_v5.h"

#include "scumm/actor.h"
#include "scumm/object.h"
#include "scumm/resource.h"

#include "common/array.h"
#include "common/endian.h"
#include "common/file.h"
#include "common/hashmap.h"
#include "common/random.h"
#include "common/system.h"
#include "common/textconsole.h"

namespace Scumm {

namespace {

static const byte XOR_KEY = 0x69;
static const byte V5_PARAM_1 = 0x80;
static const byte V5_PARAM_2 = 0x40;

struct ObjCatalogEntry {
	uint16 objId;
	byte roomId;
	Common::Array<byte> cdhdBlock;
	Common::Array<byte> verbBlock;
	Common::Array<byte> obnaBlock;
	Common::Array<byte> fullObcd;
	Common::String name;
	bool isPickuppable;

	struct VerbEntry {
		byte verbId;
		uint16 offset;
	};
	Common::Array<VerbEntry> verbTable;

	ObjCatalogEntry() : objId(0), roomId(0), isPickuppable(false) {}
};

static Common::Array<ObjCatalogEntry::VerbEntry> parseVerbTable(const byte *verbBlock) {
	Common::Array<ObjCatalogEntry::VerbEntry> entries;
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

static bool validatePickupCandidate(const byte *verbBlock, uint16 pos, uint16 blockEnd, byte op, int numRooms) {
	if (op == 0x25) {
		if (pos + 3 >= blockEnd)
			return false;
		byte room = verbBlock[pos + 3];
		return room == 0 || (room >= 1 && room <= numRooms);
	} else if (op == 0x65) {
		if (pos + 4 >= blockEnd)
			return false;
		uint16 varIdx = READ_LE_UINT16(verbBlock + pos + 3);
		return varIdx < 0x4000;
	}
	return false;
}

static uint16 consumeV5VarRefSize(const byte *verbBlock, uint16 pos, uint16 end) {
	if (pos + 1 >= end)
		return 0;

	uint16 consumed = 2;
	uint16 var = READ_LE_UINT16(verbBlock + pos);
	pos += 2;

	if (var & 0x2000) {
		if (pos + 1 >= end)
			return 0;
		consumed += 2;
		uint16 idx = READ_LE_UINT16(verbBlock + pos);
		pos += 2;
		if (idx & 0x2000) {
			if (pos + 1 >= end)
				return 0;
			consumed += 2;
		}
	}

	return consumed;
}

static uint16 consumeV5VarOrDirectByteSize(const byte *verbBlock, uint16 pos, uint16 end, byte op, byte mask) {
	if (op & mask)
		return consumeV5VarRefSize(verbBlock, pos, end);
	return (pos < end) ? 1 : 0;
}

static uint16 consumeV5VarOrDirectWordSize(const byte *verbBlock, uint16 pos, uint16 end, byte op, byte mask) {
	if (op & mask)
		return consumeV5VarRefSize(verbBlock, pos, end);
	return (pos + 1 < end) ? 2 : 0;
}

static uint16 consumeV5Instruction(const byte *verbBlock, uint16 pos, uint16 end, int numRooms, Common::Array<uint16> *outTargets) {
	if (pos >= end)
		return 0;

	const byte op = verbBlock[pos];
	const byte pickupOpcodes[] = {0x25, 0x65, 0xA5, 0xE5};

	for (int i = 0; i < ARRAYSIZE(pickupOpcodes); i++) {
		if (op == pickupOpcodes[i]) {
			uint16 p = pos + 1;
			uint16 objSize = consumeV5VarOrDirectWordSize(verbBlock, p, end, op, V5_PARAM_1);
			if (objSize == 0)
				return 1;
			p += objSize;
			uint16 roomSize = consumeV5VarOrDirectByteSize(verbBlock, p, end, op, V5_PARAM_2);
			if (roomSize == 0)
				return 1;

			if ((op == 0x25 || op == 0x65) && outTargets && validatePickupCandidate(verbBlock, pos, end, op, numRooms)) {
				outTargets->push_back(READ_LE_UINT16(verbBlock + pos + 1));
			}

			return 1 + objSize + roomSize;
		}
	}

	if (op == 0x3C || op == 0xBC) {
		uint16 argSize = consumeV5VarOrDirectByteSize(verbBlock, pos + 1, end, op, V5_PARAM_1);
		return (argSize == 0) ? 1 : (1 + argSize);
	}

	return 1;
}

static bool verbScriptHasPickup(const byte *verbBlock, uint32 verbBlockSize, uint16 scriptOffset, uint16 nextOffset, int numRooms) {
	if (scriptOffset >= verbBlockSize)
		return false;
	uint16 end = (nextOffset > 0 && nextOffset <= verbBlockSize) ? nextOffset : verbBlockSize;

	uint16 pos = scriptOffset;
	while (pos < end) {
		Common::Array<uint16> targets;
		uint16 consumed = consumeV5Instruction(verbBlock, pos, end, numRooms, &targets);
		if (!targets.empty())
			return true;
		if (consumed == 0)
			break;
		pos += consumed;
	}

	return false;
}

static void getVerbScriptRange(const Common::Array<ObjCatalogEntry::VerbEntry> &table, int idx, uint32 verbBlockSize, uint16 &outStart, uint16 &outEnd) {
	outStart = table[idx].offset;
	outEnd = (uint16)verbBlockSize;
	for (int j = 0; j < (int)table.size(); j++) {
		if (j == idx)
			continue;
		if (table[j].offset > outStart && table[j].offset < outEnd)
			outEnd = table[j].offset;
	}
}

static Common::Array<byte> buildMergedVerbBlock(const ObjCatalogEntry &slot, const ObjCatalogEntry &donor, int numRooms) {
	const byte *slotVerb = slot.verbBlock.data();
	uint32 slotVerbSize = slot.verbBlock.size();
	const byte *donorVerb = donor.verbBlock.data();
	uint32 donorVerbSize = donor.verbBlock.size();

	Common::Array<ObjCatalogEntry::VerbEntry> slotTable = parseVerbTable(slotVerb);
	Common::Array<ObjCatalogEntry::VerbEntry> donorTable = parseVerbTable(donorVerb);

	Common::Array<byte> pickupVerbIds;
	for (int i = 0; i < (int)slotTable.size(); i++) {
		uint16 start, end;
		getVerbScriptRange(slotTable, i, slotVerbSize, start, end);
		if (verbScriptHasPickup(slotVerb, slotVerbSize, start, end, numRooms))
			pickupVerbIds.push_back(slotTable[i].verbId);
	}

	struct MergedVerbScript {
		byte verbId;
		Common::Array<byte> scriptBytes;
	};
	Common::Array<MergedVerbScript> merged;

	for (int i = 0; i < (int)donorTable.size(); i++) {
		bool isPickup = false;
		for (int k = 0; k < (int)pickupVerbIds.size(); k++) {
			if (donorTable[i].verbId == pickupVerbIds[k]) {
				isPickup = true;
				break;
			}
		}
		if (isPickup)
			continue;

		uint16 start, end;
		getVerbScriptRange(donorTable, i, donorVerbSize, start, end);

		MergedVerbScript mv;
		mv.verbId = donorTable[i].verbId;
		mv.scriptBytes.resize(end - start);
		memcpy(mv.scriptBytes.data(), donorVerb + start, end - start);

		for (uint32 j = 0; j + 1 < mv.scriptBytes.size(); j++) {
			uint16 value = READ_LE_UINT16(mv.scriptBytes.data() + j);
			if (value == donor.objId)
				WRITE_LE_UINT16(mv.scriptBytes.data() + j, slot.objId);
		}

		merged.push_back(mv);
	}

	for (int k = 0; k < (int)pickupVerbIds.size(); k++) {
		byte pickupVerbId = pickupVerbIds[k];
		for (int i = 0; i < (int)slotTable.size(); i++) {
			if (slotTable[i].verbId != pickupVerbId)
				continue;

			uint16 start, end;
			getVerbScriptRange(slotTable, i, slotVerbSize, start, end);

			MergedVerbScript mv;
			mv.verbId = pickupVerbId;
			mv.scriptBytes.resize(end - start);
			memcpy(mv.scriptBytes.data(), slotVerb + start, end - start);
			merged.push_back(mv);
			break;
		}
	}

	uint32 tableSize = merged.size() * 3 + 1;
	uint32 scriptDataSize = 0;
	for (int i = 0; i < (int)merged.size(); i++)
		scriptDataSize += merged[i].scriptBytes.size();

	uint32 verbDataSize = tableSize + scriptDataSize;
	uint32 totalVerbSize = 8 + verbDataSize;

	Common::Array<byte> result(totalVerbSize);
	byte *out = result.data();

	WRITE_BE_UINT32(out, MKTAG('V', 'E', 'R', 'B'));
	WRITE_BE_UINT32(out + 4, totalVerbSize);

	byte *tablePtr = out + 8;
	uint16 scriptOffset = 8 + tableSize;
	for (int i = 0; i < (int)merged.size(); i++) {
		tablePtr[0] = merged[i].verbId;
		WRITE_LE_UINT16(tablePtr + 1, scriptOffset);
		tablePtr += 3;
		scriptOffset += merged[i].scriptBytes.size();
	}
	*tablePtr = 0x00;

	byte *scriptPtr = out + 8 + tableSize;
	for (int i = 0; i < (int)merged.size(); i++) {
		memcpy(scriptPtr, merged[i].scriptBytes.data(), merged[i].scriptBytes.size());
		scriptPtr += merged[i].scriptBytes.size();
	}

	return result;
}

static Common::Array<byte> buildMergedObcd(const ObjCatalogEntry &slot, const ObjCatalogEntry &donor, int numRooms) {
	Common::Array<byte> mergedVerb = buildMergedVerbBlock(slot, donor, numRooms);

	uint32 obcdDataSize = slot.cdhdBlock.size() + mergedVerb.size() + donor.obnaBlock.size();
	uint32 totalObcdSize = 8 + obcdDataSize;

	Common::Array<byte> result(totalObcdSize);
	byte *out = result.data();

	WRITE_BE_UINT32(out, MKTAG('O', 'B', 'C', 'D'));
	WRITE_BE_UINT32(out + 4, totalObcdSize);

	byte *ptr = out + 8;
	memcpy(ptr, slot.cdhdBlock.data(), slot.cdhdBlock.size());
	ptr += slot.cdhdBlock.size();
	memcpy(ptr, mergedVerb.data(), mergedVerb.size());
	ptr += mergedVerb.size();
	memcpy(ptr, donor.obnaBlock.data(), donor.obnaBlock.size());

	return result;
}

static Common::Array<byte> extractBlock(const byte *parent, uint32 parentSize, uint32 tag) {
	uint32 pos = 8;
	while (pos + 8 <= parentSize) {
		uint32 childTag = READ_BE_UINT32(parent + pos);
		uint32 childSize = READ_BE_UINT32(parent + pos + 4);
		if (childSize == 0 || childSize > parentSize - pos)
			break;
		if (childTag == tag) {
			Common::Array<byte> result(childSize);
			memcpy(result.data(), parent + pos, childSize);
			return result;
		}
		pos += childSize;
	}
	return Common::Array<byte>();
}

static Common::String getObjName(const Common::Array<byte> &obnaBlock) {
	if (obnaBlock.size() <= 8)
		return Common::String();
	return Common::String((const char *)(obnaBlock.data() + 8));
}

static void xorBuffer(byte *buf, uint32 size, byte key) {
	for (uint32 i = 0; i < size; i++)
		buf[i] ^= key;
}

} // namespace

Randomizer_v5::Randomizer_v5(OSystem *syst, const DetectorResult &dr)
	: ScummEngine_v5(syst, dr) {
}

void Randomizer_v5::setupOpcodes() {
	ScummEngine_v5::setupOpcodes();
	_opcodes[0x25]._OPCODE(Randomizer_v5, o5_pickupObject);
	_opcodes[0x65]._OPCODE(Randomizer_v5, o5_pickupObject);
	_opcodes[0xA5]._OPCODE(Randomizer_v5, o5_pickupObject);
	_opcodes[0xE5]._OPCODE(Randomizer_v5, o5_pickupObject);

	const byte loadRoomOps[] = {0x72, 0xF2};
	const byte loadRoomWithEgoOps[] = {0x24, 0x64, 0xA4, 0xE4};
	const byte putActorOps[] = {0x01, 0x21, 0x41, 0x61, 0x81, 0xA1, 0xC1, 0xE1};
	const byte putActorAtObjectOps[] = {0x0E, 0x4E, 0x8E, 0xCE};
	const byte putActorInRoomOps[] = {0x2D, 0x6D, 0xAD, 0xED};
	const byte walkActorToOps[] = {0x1E, 0x3E, 0x5E, 0x7E, 0x9E, 0xBE, 0xDE, 0xFE};
	const byte walkActorToActorOps[] = {0x0D, 0x4D, 0x8D, 0xCD};
	const byte walkActorToObjectOps[] = {0x36, 0x76, 0xB6, 0xF6};

	for (int i = 0; i < ARRAYSIZE(loadRoomOps); i++)
		_opcodes[loadRoomOps[i]]._OPCODE(Randomizer_v5, o5_loadRoom);
	for (int i = 0; i < ARRAYSIZE(loadRoomWithEgoOps); i++)
		_opcodes[loadRoomWithEgoOps[i]]._OPCODE(Randomizer_v5, o5_loadRoomWithEgo);
	for (int i = 0; i < ARRAYSIZE(putActorOps); i++)
		_opcodes[putActorOps[i]]._OPCODE(Randomizer_v5, o5_putActor);
	for (int i = 0; i < ARRAYSIZE(putActorAtObjectOps); i++)
		_opcodes[putActorAtObjectOps[i]]._OPCODE(Randomizer_v5, o5_putActorAtObject);
	for (int i = 0; i < ARRAYSIZE(putActorInRoomOps); i++)
		_opcodes[putActorInRoomOps[i]]._OPCODE(Randomizer_v5, o5_putActorInRoom);
	for (int i = 0; i < ARRAYSIZE(walkActorToOps); i++)
		_opcodes[walkActorToOps[i]]._OPCODE(Randomizer_v5, o5_walkActorTo);
	for (int i = 0; i < ARRAYSIZE(walkActorToActorOps); i++)
		_opcodes[walkActorToActorOps[i]]._OPCODE(Randomizer_v5, o5_walkActorToActor);
	for (int i = 0; i < ARRAYSIZE(walkActorToObjectOps); i++)
		_opcodes[walkActorToObjectOps[i]]._OPCODE(Randomizer_v5, o5_walkActorToObject);
}

void Randomizer_v5::resetDiscoveryState() {
	for (int i = 0; i < NUM_SCRIPT_SLOT; i++) {
		vm.slot[i].offs = 0;
		vm.slot[i].delay = 0;
		vm.slot[i].number = 0;
		vm.slot[i].delayFrameCount = 0;
		vm.slot[i].freezeResistant = false;
		vm.slot[i].recursive = false;
		vm.slot[i].didexec = false;
		vm.slot[i].status = ssDead;
		vm.slot[i].where = 0;
		vm.slot[i].freezeCount = 0;
		vm.slot[i].cutsceneOverride = 0;
		vm.slot[i].cycle = 1;
	}

	_currentScript = 0xFF;
	_scummStackPos = 0;
	vm.numNestedScripts = 0;
	vm.cutSceneStackPointer = 0;
	vm.cutSceneScriptIndex = 0xFF;
	memset(vm.cutSceneScript, 0, sizeof(vm.cutSceneScript));
	memset(vm.cutScenePtr, 0, sizeof(vm.cutScenePtr));
	memset(vm.cutSceneData, 0, sizeof(vm.cutSceneData));
	memset(_localScriptOffsets, 0, sizeof(_localScriptOffsets));
	_sentenceNum = 0;
	_haveMsg = 0;
	_talkDelay = 0;

	if (VAR_EGO != 0xFF)
		VAR(VAR_EGO) = 1;
	if (VAR_ROOM != 0xFF)
		VAR(VAR_ROOM) = 0;
	if (VAR_ROOM_RESOURCE != 0xFF)
		VAR(VAR_ROOM_RESOURCE) = 0;
	if (VAR_HAVE_MSG != 0xFF)
		VAR(VAR_HAVE_MSG) = 0;
	if (VAR_OVERRIDE != 0xFF)
		VAR(VAR_OVERRIDE) = 0;
	if (VAR_CURRENT_LIGHTS != 0xFF)
		VAR(VAR_CURRENT_LIGHTS) = 1;
}

void Randomizer_v5::prepareRoomForDiscovery(int roomId) {
	_roomResource = roomId;
	_currentRoom = roomId;

	if (VAR_ROOM != 0xFF)
		VAR(VAR_ROOM) = roomId;
	if (VAR_ROOM_RESOURCE != 0xFF)
		VAR(VAR_ROOM_RESOURCE) = roomId;

	openRoom(roomId);
	clearRoomObjects();
	setupRoomSubBlocks();
	resetRoomSubBlocks();
	resetRoomObjects();

	Actor *ego = derefActorSafe(1, "Randomizer_v5::prepareRoomForDiscovery");
	if (ego)
		ego->putActor(0, 0, roomId);
}

void Randomizer_v5::executeScriptAtOffset(uint16 scriptNumber, uint32 scriptOffset, byte where, int *vars) {
	int slot = getScriptSlot();
	ScriptSlot *scriptSlot = &vm.slot[slot];

	scriptSlot->number = scriptNumber;
	scriptSlot->offs = scriptOffset;
	scriptSlot->status = ssRunning;
	scriptSlot->where = where;
	scriptSlot->freezeResistant = false;
	scriptSlot->recursive = false;
	scriptSlot->freezeCount = 0;
	scriptSlot->delayFrameCount = 0;
	scriptSlot->cycle = 1;

	initializeLocals(slot, vars);
	runScriptNested(slot);
}

void Randomizer_v5::o5_pickupObject() {
	int obj = getVarOrDirectWord(PARAM_1);
	int room = getVarOrDirectByte(PARAM_2);
	if (room == 0)
		room = _roomResource;

	PickupCall call;
	call.objectId = obj;
	call.room = room;
	call.scriptNum = (_currentScript != 0xFF) ? vm.slot[_currentScript].number : 0;
	call.scriptOffset = (_currentScript != 0xFF) ? (uint16)(_scriptPointer - _scriptOrgPointer) : 0;
	_pickupCalls.push_back(call);

	runInventoryScript(1);
}

void Randomizer_v5::o5_loadRoom() {
	(void)getVarOrDirectByte(PARAM_1);
}

void Randomizer_v5::o5_loadRoomWithEgo() {
	(void)getVarOrDirectWord(PARAM_1);
	(void)getVarOrDirectByte(PARAM_2);
	(void)fetchScriptWordSigned();
	(void)fetchScriptWordSigned();
}

void Randomizer_v5::o5_putActor() {
	(void)getVarOrDirectByte(PARAM_1);
	(void)getVarOrDirectWord(PARAM_2);
	(void)getVarOrDirectWord(PARAM_3);
}

void Randomizer_v5::o5_putActorAtObject() {
	(void)getVarOrDirectByte(PARAM_1);
	(void)getVarOrDirectWord(PARAM_2);
}

void Randomizer_v5::o5_putActorInRoom() {
	(void)getVarOrDirectByte(PARAM_1);
	(void)getVarOrDirectByte(PARAM_2);
}

void Randomizer_v5::o5_walkActorTo() {
	(void)getVarOrDirectByte(PARAM_1);
	(void)getVarOrDirectWord(PARAM_2);
	(void)getVarOrDirectWord(PARAM_3);
}

void Randomizer_v5::o5_walkActorToActor() {
	(void)getVarOrDirectByte(PARAM_1);
	(void)getVarOrDirectByte(PARAM_2);
	(void)fetchScriptByte();
}

void Randomizer_v5::o5_walkActorToObject() {
	(void)getVarOrDirectByte(PARAM_1);
	(void)getVarOrDirectWord(PARAM_2);
}

Common::Error Randomizer_v5::run() {
	Common::Error err;
	err = init();
	if (err.getCode() != Common::kNoError)
		return err;
	return randomize();
}

Common::Error Randomizer_v5::randomize() {
	debug(0, "=== SCUMM V5 OBCD Swap Randomizer ===");

	Common::Array<ObjCatalogEntry> catalog;
	Common::HashMap<uint16, int> objToCatalogIdx;

	debug(0, "Scanning %d rooms for objects...", _numRooms);

	for (int roomId = 1; roomId < _numRooms; roomId++) {
		if (_res->_types[rtRoom][roomId]._roomoffs == RES_INVALID_OFFSET)
			continue;
		if (_res->_types[rtRoom][roomId]._roomoffs == 0 && roomId != 0)
			continue;

		const byte *roomPtr = getResourceAddress(rtRoom, roomId);
		if (!roomPtr)
			continue;

		const byte *rmhd = findResourceData(MKTAG('R', 'M', 'H', 'D'), roomPtr);
		if (!rmhd)
			continue;
		int numObjects = READ_LE_UINT16(&((const RoomHeader *)rmhd)->old.numObjects);
		if (numObjects == 0)
			continue;

		ResourceIterator obcds(roomPtr, false);
		for (int objectIndex = 0; objectIndex < numObjects; objectIndex++) {
			const byte *obcdPtr = obcds.findNext(MKTAG('O', 'B', 'C', 'D'));
			if (!obcdPtr)
				break;

			uint32 obcdSize = READ_BE_UINT32(obcdPtr + 4);

			ObjCatalogEntry entry;
			entry.roomId = (byte)roomId;
			entry.fullObcd.resize(obcdSize);
			memcpy(entry.fullObcd.data(), obcdPtr, obcdSize);
			entry.cdhdBlock = extractBlock(obcdPtr, obcdSize, MKTAG('C', 'D', 'H', 'D'));
			entry.verbBlock = extractBlock(obcdPtr, obcdSize, MKTAG('V', 'E', 'R', 'B'));
			entry.obnaBlock = extractBlock(obcdPtr, obcdSize, MKTAG('O', 'B', 'N', 'A'));

			if (entry.cdhdBlock.size() >= 10)
				entry.objId = READ_LE_UINT16(entry.cdhdBlock.data() + 8);

			entry.name = getObjName(entry.obnaBlock);
			if (!entry.verbBlock.empty())
				entry.verbTable = parseVerbTable(entry.verbBlock.data());

			int idx = catalog.size();
			catalog.push_back(entry);
			objToCatalogIdx[entry.objId] = idx;

			debug(1, "  Room %d: obj %d \"%s\"", roomId, entry.objId, entry.name.c_str());
		}
	}

	_pickupCalls.clear();
	resetDiscoveryState();

	int zeroArgs[NUM_SCRIPT_LOCAL];
	memset(zeroArgs, 0, sizeof(zeroArgs));

	_currentRoom = 0;
	_roomResource = 0;
	if (VAR_ROOM != 0xFF)
		VAR(VAR_ROOM) = 0;
	if (VAR_ROOM_RESOURCE != 0xFF)
		VAR(VAR_ROOM_RESOURCE) = 0;

	Actor *ego = derefActorSafe(1, "Randomizer_v5::go");
	if (ego)
		ego->putActor(0, 0, 0);

	for (int scriptId = 1; scriptId < _numGlobalScripts; scriptId++) {
		if (_res->_types[rtScript][scriptId]._roomoffs == RES_INVALID_OFFSET)
			continue;
		if (!getResourceAddress(rtScript, scriptId))
			continue;
		runScript(scriptId, false, false, zeroArgs);
	}

	for (int roomId = 1; roomId < _numRooms; roomId++) {
		if (_res->_types[rtRoom][roomId]._roomoffs == RES_INVALID_OFFSET)
			continue;
		if (_res->_types[rtRoom][roomId]._roomoffs == 0 && roomId != 0)
			continue;

		prepareRoomForDiscovery(roomId);

		for (int localIndex = 0; localIndex < _numLocalScripts; localIndex++) {
			if (_localScriptOffsets[localIndex] == 0)
				continue;
			runScript(_numGlobalScripts + localIndex, false, false, zeroArgs);
		}

		const byte *roomPtr = getResourceAddress(rtRoom, roomId);
		if (!roomPtr)
			continue;

		const byte *rmhd = findResourceData(MKTAG('R', 'M', 'H', 'D'), roomPtr);
		if (!rmhd)
			continue;
		int numObjects = READ_LE_UINT16(&((const RoomHeader *)rmhd)->old.numObjects);
		if (numObjects == 0)
			continue;

		ResourceIterator obcds(roomPtr, false);
		for (int objectIndex = 0; objectIndex < numObjects; objectIndex++) {
			const byte *obcdPtr = obcds.findNext(MKTAG('O', 'B', 'C', 'D'));
			if (!obcdPtr)
				break;

			const byte *cdhd = findResourceData(MKTAG('C', 'D', 'H', 'D'), obcdPtr);
			const byte *verb = findResource(MKTAG('V', 'E', 'R', 'B'), obcdPtr);
			if (!cdhd || !verb)
				continue;

			uint16 objId = READ_LE_UINT16(cdhd + 0);
			Common::Array<ObjCatalogEntry::VerbEntry> verbTable = parseVerbTable(verb);
			uint32 verbBaseOffset = verb - roomPtr;

			for (int verbIndex = 0; verbIndex < (int)verbTable.size(); verbIndex++) {
				executeScriptAtOffset(objId, verbBaseOffset + verbTable[verbIndex].offset, WIO_ROOM, zeroArgs);
			}
		}
	}

	Common::HashMap<uint16, bool> pickuppableObjIds;
	for (int i = 0; i < (int)_pickupCalls.size(); i++) {
		pickuppableObjIds[_pickupCalls[i].objectId] = true;
	}

	for (Common::HashMap<uint16, bool>::iterator it = pickuppableObjIds.begin(); it != pickuppableObjIds.end(); ++it) {
		uint16 targetId = it->_key;
		if (objToCatalogIdx.contains(targetId)) {
			catalog[objToCatalogIdx[targetId]].isPickuppable = true;
			debug(1, "  Marked obj %d as pickuppable", targetId);
		} else {
			debug(1, "  pickupObject target obj %d not found in any room OBCD", targetId);
		}
	}

	Common::Array<int> pickuppableIndices;
	for (int i = 0; i < (int)catalog.size(); i++) {
		if (catalog[i].isPickuppable)
			pickuppableIndices.push_back(i);
	}

	debug(0, "Total objects: %d, pick-uppable: %d", catalog.size(), pickuppableIndices.size());
	for (int i = 0; i < (int)pickuppableIndices.size(); i++) {
		const ObjCatalogEntry &entry = catalog[pickuppableIndices[i]];
		debug(0, "  Pickuppable: obj %d in room %d: \"%s\"", entry.objId, entry.roomId, entry.name.c_str());
	}

	if (pickuppableIndices.size() < 2) {
		warning("randomizer: Not enough pickuppable objects to shuffle");
		return Common::kNoError;
	}

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

	Common::HashMap<uint16, Common::Array<byte>> newObcds;
	for (int i = 0; i < (int)shuffled.size(); i++) {
		int slotIdx = pickuppableIndices[i];
		int donorIdx = pickuppableIndices[shuffled[i]];
		if (slotIdx == donorIdx)
			continue;

		const ObjCatalogEntry &slot = catalog[slotIdx];
		const ObjCatalogEntry &donor = catalog[donorIdx];
		Common::Array<byte> merged = buildMergedObcd(slot, donor, _numRooms);
		newObcds[slot.objId] = merged;

		debug(0, "  Built merged OBCD for slot obj %d (size %u -> %u)", slot.objId, slot.fullObcd.size(), merged.size());
	}

	Common::HashMap<byte, bool> modifiedRooms;
	for (Common::HashMap<uint16, Common::Array<byte>>::iterator it = newObcds.begin(); it != newObcds.end(); ++it) {
		uint16 objId = it->_key;
		if (objToCatalogIdx.contains(objId))
			modifiedRooms[catalog[objToCatalogIdx[objId]].roomId] = true;
	}

	Common::HashMap<byte, Common::Array<byte>> roomNewBlocks;
	for (int roomId = 1; roomId < _numRooms; roomId++) {
		if (!modifiedRooms.contains((byte)roomId))
			continue;

		const byte *roomPtr = getResourceAddress(rtRoom, roomId);
		if (!roomPtr)
			continue;

		uint32 roomSize = READ_BE_UINT32(roomPtr + 4);
		Common::Array<byte> newRoomData;
		uint32 pos = 8;
		while (pos + 8 <= roomSize) {
			uint32 childTag = READ_BE_UINT32(roomPtr + pos);
			uint32 childSize = READ_BE_UINT32(roomPtr + pos + 4);
			if (childSize == 0 || childSize > roomSize - pos)
				break;

			if (childTag == MKTAG('O', 'B', 'C', 'D')) {
				uint16 objId = 0;
				Common::Array<byte> cdhdBlock = extractBlock(roomPtr + pos, childSize, MKTAG('C', 'D', 'H', 'D'));
				if (cdhdBlock.size() >= 10)
					objId = READ_LE_UINT16(cdhdBlock.data() + 8);

				if (newObcds.contains(objId)) {
					const Common::Array<byte> &merged = newObcds[objId];
					for (uint32 b = 0; b < merged.size(); b++)
						newRoomData.push_back(merged[b]);
					debug(1, "  Room %d: replaced OBCD for obj %d (old size %u, new size %u)", roomId, objId, childSize, merged.size());
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

		uint32 newRoomSize = 8 + newRoomData.size();
		Common::Array<byte> newRoom(newRoomSize);
		WRITE_BE_UINT32(newRoom.data(), MKTAG('R', 'O', 'O', 'M'));
		WRITE_BE_UINT32(newRoom.data() + 4, newRoomSize);
		memcpy(newRoom.data() + 8, newRoomData.data(), newRoomData.size());
		roomNewBlocks[(byte)roomId] = newRoom;

		debug(0, "  Rebuilt room %d: %u -> %u bytes", roomId, roomSize, newRoomSize);
	}

	struct RoomOffset {
		byte roomId;
		uint32 offset;
	};

	Common::Path dataFilename(generateFilename(1));
	Common::File rawFile;
	if (!rawFile.open(dataFilename)) {
		warning("randomizer: Cannot open data file %s", dataFilename.toString().c_str());
		return Common::kPathNotFile;
	}

	uint32 rawSize = rawFile.size();
	Common::Array<byte> rawData(rawSize);
	rawFile.read(rawData.data(), rawSize);
	rawFile.close();
	xorBuffer(rawData.data(), rawData.size(), XOR_KEY);

	const byte *d001 = rawData.data();
	uint32 d001Size = rawData.size();
	if (READ_BE_UINT32(d001) != MKTAG('L', 'E', 'C', 'F')) {
		warning("randomizer: Data file does not start with LECF");
		return Common::kUnknownError;
	}
	if (READ_BE_UINT32(d001 + 8) != MKTAG('L', 'O', 'F', 'F')) {
		warning("randomizer: LOFF not found at expected offset");
		return Common::kUnknownError;
	}

	byte numFileRooms = d001[16];
	Common::Array<RoomOffset> roomOffsets;
	uint32 loffPos = 17;
	for (int i = 0; i < numFileRooms; i++) {
		RoomOffset ro;
		ro.roomId = d001[loffPos];
		ro.offset = READ_LE_UINT32(d001 + loffPos + 1);
		roomOffsets.push_back(ro);
		loffPos += 5;
	}

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
	for (int i = 0; i < (int)roomOffsets.size(); i++) {
		byte roomId = roomOffsets[i].roomId;
		uint32 origRoomOffset = roomOffsets[i].offset;

		NewRoomInfo info;
		info.roomId = roomId;
		if (roomNewBlocks.contains(roomId)) {
			info.roomBlock = roomNewBlocks[roomId];
		} else if (origRoomOffset + 8 <= d001Size && READ_BE_UINT32(d001 + origRoomOffset) == MKTAG('R', 'O', 'O', 'M')) {
			uint32 origSize = READ_BE_UINT32(d001 + origRoomOffset + 4);
			info.roomBlock.resize(origSize);
			memcpy(info.roomBlock.data(), d001 + origRoomOffset, origSize);
		}

		uint32 lflfOff = origRoomOffset - 8;
		if (lflfOff < d001Size && READ_BE_UINT32(d001 + lflfOff) == MKTAG('L', 'F', 'L', 'F')) {
			uint32 lflfSize = READ_BE_UINT32(d001 + lflfOff + 4);
			uint32 lflfEnd = lflfOff + lflfSize;
			uint32 origRoomSize = READ_BE_UINT32(d001 + origRoomOffset + 4);
			uint32 extraStart = origRoomOffset + origRoomSize;
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
	WRITE_BE_UINT32(out, MKTAG('L', 'E', 'C', 'F'));
	WRITE_BE_UINT32(out + 4, lecfTotalSize);

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

	for (int i = 0; i < (int)newRoomInfos.size(); i++) {
		const NewRoomInfo &info = newRoomInfos[i];
		byte *lflfPtr = out + info.lflfOffset;
		uint32 lflfContentSize = info.roomBlock.size() + info.extraBlocks.size();
		uint32 lflfTotalSize = 8 + lflfContentSize;

		WRITE_BE_UINT32(lflfPtr, MKTAG('L', 'F', 'L', 'F'));
		WRITE_BE_UINT32(lflfPtr + 4, lflfTotalSize);
		memcpy(lflfPtr + 8, info.roomBlock.data(), info.roomBlock.size());
		if (!info.extraBlocks.empty())
			memcpy(lflfPtr + 8 + info.roomBlock.size(), info.extraBlocks.data(), info.extraBlocks.size());
	}

	xorBuffer(output.data(), output.size(), XOR_KEY);
	Common::DumpFile outFile;
	if (!outFile.open(Common::Path("output/MONKEY2/MONKEY2.001"), true)) {
		warning("randomizer: Cannot open output .001 file");
		return Common::kWritingFailed;
	}
	outFile.write(output.data(), output.size());
	outFile.close();
	debug(0, "Wrote .001 (%u bytes)", output.size());

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
	xorBuffer(newIndex.data(), newIndex.size(), XOR_KEY);

	uint32 idxPos = 0;
	while (idxPos + 8 < newIndex.size()) {
		uint32 blockTag = READ_BE_UINT32(newIndex.data() + idxPos);
		uint32 blockSize = READ_BE_UINT32(newIndex.data() + idxPos + 4);
		if (blockSize == 0)
			break;

		if (blockTag == MKTAG('D', 'R', 'O', 'O')) {
			uint16 num = READ_LE_UINT16(newIndex.data() + idxPos + 8);
			uint32 roomOffsStart = idxPos + 8 + 2 + num;

			for (int i = 0; i < (int)newRoomInfos.size(); i++) {
				byte roomId = newRoomInfos[i].roomId;
				if (roomId < num) {
					WRITE_LE_UINT32(newIndex.data() + roomOffsStart + roomId * 4, newRoomInfos[i].roomOffset);
				}
			}

			debug(0, "Patched DROO with %d room offsets", newRoomInfos.size());
			break;
		}

		idxPos += blockSize;
	}

	xorBuffer(newIndex.data(), newIndex.size(), XOR_KEY);
	Common::DumpFile idxFile;
	if (!idxFile.open(Common::Path("output/MONKEY2/MONKEY2.000"), true)) {
		warning("randomizer: Cannot open output .000 file");
		return Common::kWritingFailed;
	}
	idxFile.write(newIndex.data(), newIndex.size());
	idxFile.close();
	debug(0, "Wrote .000 (%u bytes)", newIndex.size());

	debug(0, "=== Randomization complete! ===");
	return Common::kNoError;
}

} // End of namespace Scumm