#include "scumm/randomizer_v5.h"

#include "scumm/actor.h"
#include "scumm/object.h"
#include "scumm/resource.h"
#include "scumm/verbs.h"

#include "common/array.h"
#include "common/endian.h"
#include "common/events.h"
#include "common/file.h"
#include "common/hashmap.h"
#include "common/random.h"
#include "common/system.h"
#include "common/textconsole.h"

namespace Scumm {

#define OPCODE(i, x) _opcodes[i]._OPCODE(Randomizer_v5, x)

namespace {

static const byte XOR_KEY = 0x69;
static const byte V5_PARAM_1 = 0x80;
static const byte V5_PARAM_2 = 0x40;

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
		byte op = verbBlock[pos];
		if (op == 0x00)
			break;

		const byte pickupOpcodes[] = {0x25, 0x65, 0xA5, 0xE5};
		for (int i = 0; i < ARRAYSIZE(pickupOpcodes); i++) {
			if (op == pickupOpcodes[i])
				return true;
		}

		uint16 consumed = consumeV5Instruction(verbBlock, pos, end, numRooms, nullptr);
		if (consumed == 0)
			break;

		// The lightweight scanner only understands a subset of v5 opcodes.
		// If it cannot confidently step over an opcode, stop rather than
		// searching arbitrary operand bytes and producing false positives.
		if (consumed == 1 && op != 0x80)
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
	: ScummEngine_v5(syst, dr), _discoveryRecursionDepth(0), _discoveryDrainCount(0), _nextDiscoveryCheckpointSlot(DISCOVERY_CHECKPOINT_FIRST_SLOT) {
}

void Randomizer_v5::setupScumm(const Common::Path &macResourceFile) {
	ScummEngine_v5::setupScumm(macResourceFile);
}

void Randomizer_v5::setupOpcodes() {
	ScummEngine_v5::setupOpcodes();
	OPCODE(0x25, o5_pickupObject);
	OPCODE(0x65, o5_pickupObject);
	OPCODE(0xA5, o5_pickupObject);
	OPCODE(0xE5, o5_pickupObject);
}

void Randomizer_v5::snapshotDiscoveryState(DiscoveryStateSnapshot &out) {
	out.roomId = _currentRoom;
	out.roomResource = _roomResource;
	out.hash = hashDiscoveryState(out);
}

void Randomizer_v5::checkpointDiscoveryState() {
	Common::String filename;
	if (!saveState(_nextDiscoveryCheckpointSlot, true, filename)) {
		warning("randomizer: unable to save discovery checkpoint in slot %d", _nextDiscoveryCheckpointSlot);
		return;
	}
	debug(1, "Discovery checkpoint saved in slot %d", _nextDiscoveryCheckpointSlot);
	_nextDiscoveryCheckpointSlot++;
	if (_nextDiscoveryCheckpointSlot >= DISCOVERY_CHECKPOINT_FIRST_SLOT + DISCOVERY_CHECKPOINT_SLOTS)
		_nextDiscoveryCheckpointSlot = DISCOVERY_CHECKPOINT_FIRST_SLOT;
}

uint32 Randomizer_v5::hashDiscoveryState(const DiscoveryStateSnapshot &state) const {
	uint32 hash = 2166136261u;
	hash ^= (uint32)state.roomId;
	hash *= 16777619u;
	hash ^= (uint32)state.roomResource;
	hash *= 16777619u;
	for (int i = 0; i < _numVariables; i++) {
		hash ^= (uint32)_scummVars[i];
		hash *= 16777619u;
	}
	for (int i = 0; i < _numGlobalObjects; i++) {
		hash ^= _objectOwnerTable ? _objectOwnerTable[i] : 0;
		hash *= 16777619u;
		hash ^= _objectRoomTable ? _objectRoomTable[i] : 0;
		hash *= 16777619u;
		hash ^= _objectStateTable ? _objectStateTable[i] : 0;
		hash *= 16777619u;
	}
	for (int i = 0; i < _numInventory; i++) {
		hash ^= _inventory ? _inventory[i] : 0;
		hash *= 16777619u;
	}
	for (int i = 0; i < _numActors; i++) {
		if (!_actors[i])
			continue;
		const Common::Point &actorPos = _actors[i]->getRealPos();
		hash ^= (uint32)_actors[i]->getRoom();
		hash *= 16777619u;
		hash ^= (uint32)actorPos.x;
		hash *= 16777619u;
		hash ^= (uint32)actorPos.y;
		hash *= 16777619u;
		hash ^= (uint32)_actors[i]->getFacing();
		hash *= 16777619u;
	}
	for (int i = 0; i < NUM_SCRIPT_SLOT; i++) {
		if (vm.slot[i].status == ssDead)
			continue;
		hash ^= (uint32)vm.slot[i].number;
		hash *= 16777619u;
		hash ^= (uint32)vm.slot[i].status;
		hash *= 16777619u;
		hash ^= (uint32)vm.slot[i].delay;
		hash *= 16777619u;
		hash ^= (uint32)vm.slot[i].delayFrameCount;
		hash *= 16777619u;
		hash ^= (uint32)vm.slot[i].freezeCount;
		hash *= 16777619u;
		for (int j = 0; j < NUM_SCRIPT_LOCAL; j++) {
			hash ^= (uint32)vm.localvar[i][j];
			hash *= 16777619u;
		}
	}
	return hash;
}

void Randomizer_v5::recordDiscoveryTrace(const DiscoveryTraceStep &step) {
	if (step.parentHash != 0 &&
		(step.parentHash == step.stateHash || _discoverySeenStates.contains(step.stateHash)))
		return;
	_discoveryTrace.push_back(step);
}

bool Randomizer_v5::registerDiscoveryState(uint32 hash) {
	if (_discoverySeenStates.contains(hash))
		return true;
	if (_discoverySeenStates.size() >= MAX_DISCOVERY_STATES) {
		debug(0, "Discovery state limit reached (%u)", MAX_DISCOVERY_STATES);
		return false;
	}
	_discoverySeenStates[hash] = true;
	return true;
}

void Randomizer_v5::beginDiscoveryGraphSearch() {
	_discoveryTrace.clear();
	_discoveryInputAttempts.clear();
	_discoverySeenStates.clear();
	_discoverySeenRooms.clear();
	_nextDiscoveryCheckpointSlot = DISCOVERY_CHECKPOINT_FIRST_SLOT;

	DiscoveryStateSnapshot startState;
	snapshotDiscoveryState(startState);
	_discoverySeenStates[startState.hash] = true;
	checkpointDiscoveryState();

	DiscoveryTraceStep bootstrap;
	bootstrap.parentHash = 0;
	bootstrap.stateHash = startState.hash;
	bootstrap.roomId = startState.roomId;
	bootstrap.objectA = 0;
	bootstrap.objectB = 0;
	bootstrap.egoObject = (VAR_EGO != 0xFF) ? VAR(VAR_EGO) : 0;
	bootstrap.verbId = 0;
	bootstrap.scriptNum = 0;
	bootstrap.scriptOffset = 0;
	bootstrap.actionType = DiscoveryAction::kUnknown;
	bootstrap.actionLabel = "bootstrap";
	recordDiscoveryTrace(bootstrap);
	debug(1, "Discovery graph bootstrap: room=%d hash=%08x", startState.roomId, startState.hash);
}

void Randomizer_v5::writeDiscoveryReport(const Common::Array<ObjCatalogEntry> &catalog, const Common::Array<int> &pickuppableIndices) const {
	Common::DumpFile report;
	if (!report.open(Common::Path("output/scummvm_discovery_v5_report.txt"), true)) {
		warning("randomizer: unable to write discovery report");
		return;
	}

	report.writeString(Common::String::format("SCUMM v5 discovery report\n"));
	report.writeString(Common::String::format("states=%u\n", _discoverySeenStates.size()));
	report.writeString(Common::String::format("reachable_rooms=%u\n", _discoverySeenRooms.size()));
	report.writeString("queued_states=0 (forward exploration)\n");
	report.writeString(Common::String::format("trace_steps=%u\n", _discoveryTrace.size()));
	report.writeString(Common::String::format("user_input_attempts=%u\n", _discoveryInputAttempts.size()));
	report.writeString(Common::String::format("pickup_calls=%u\n", _pickupCalls.size()));
	report.writeString(Common::String::format("dynamic_objects=%u\n", catalog.size()));
	report.writeString(Common::String::format("pickuppable_objects=%u\n\n", pickuppableIndices.size()));
	report.writeString("opcode policy:\n");
	report.writeString("  intercepted=0x25,0x65,0xA5,0xE5 (capture then native handler)\n");
	report.writeString("  all other v5 opcodes=native handlers\n\n");

	report.writeString("reachable rooms:\n");
	for (Common::HashMap<uint16, bool>::const_iterator it = _discoverySeenRooms.begin(); it != _discoverySeenRooms.end(); ++it)
		report.writeString(Common::String::format("  room=%u\n", it->_key));

	report.writeString("traversal:\n");
	Common::HashMap<uint32, bool> reportedStates;
	for (uint i = 0; i < _discoveryTrace.size(); i++) {
		const DiscoveryTraceStep &step = _discoveryTrace[i];
		if (step.parentHash == 0 && !reportedStates.contains(step.stateHash)) {
			report.writeString(Common::String::format(
				"  state hash=%08x room=%d discovered=bootstrap\n",
				step.stateHash, step.roomId));
			reportedStates[step.stateHash] = true;
		}
		report.writeString(Common::String::format(
			"  edge parent=%08x child=%08x room=%d type=%d label=%s verb=%u objectA=%u objectB=%u ego=%u script=%u offset=%u\n",
			step.parentHash, step.stateHash, step.roomId, step.actionType, step.actionLabel.c_str(),
			step.verbId, step.objectA, step.objectB, step.egoObject, step.scriptNum, step.scriptOffset));
		if (!reportedStates.contains(step.stateHash)) {
			report.writeString(Common::String::format(
				"  state hash=%08x room=%d discovered=edge\n",
				step.stateHash, step.roomId));
			reportedStates[step.stateHash] = true;
		}
	}

	report.writeString("user input attempts:\n");
	for (uint i = 0; i < _discoveryInputAttempts.size(); i++) {
		const DiscoveryTraceStep &attempt = _discoveryInputAttempts[i];
		report.writeString(Common::String::format(
			"  label=%s before=%08x after=%08x changed=%u\n",
			attempt.actionLabel.c_str(), attempt.parentHash, attempt.stateHash,
			attempt.parentHash != attempt.stateHash ? 1 : 0));
	}

	report.writeString("pickup calls:\n");
	for (uint i = 0; i < _pickupCalls.size(); i++) {
		const PickupCall &call = _pickupCalls[i];
		report.writeString(Common::String::format(
			"  object=%u room=%u script=%u offset=%u\n",
			call.objectId, call.room, call.scriptNum, call.scriptOffset));
	}

	report.writeString("dynamic objects:\n");
	for (uint i = 0; i < catalog.size(); i++) {
		const ObjCatalogEntry &entry = catalog[i];
		const CodeHeader *cdhd = entry.cdhdBlock.size() >= 20 ? (const CodeHeader *)(entry.cdhdBlock.data() + 8) : nullptr;
		report.writeString(Common::String::format(
			"  object=%u room=%u pickuppable=%u x=%u y=%u width=%u height=%u name=%s\n",
			entry.objId, entry.roomId, entry.isPickuppable ? 1 : 0,
			cdhd ? cdhd->v5.x * 8 : 0, cdhd ? cdhd->v5.y * 8 : 0,
			cdhd ? cdhd->v5.w * 8 : 0, cdhd ? cdhd->v5.h * 8 : 0, entry.name.c_str()));
	}

	report.writeString("pickuppable objects:\n");
	for (uint i = 0; i < pickuppableIndices.size(); i++) {
		const ObjCatalogEntry &entry = catalog[pickuppableIndices[i]];
		report.writeString(Common::String::format("  object=%u room=%u name=%s\n", entry.objId, entry.roomId, entry.name.c_str()));
	}

	report.writeString("active verbs:\n");
	for (int i = 1; i < _numVerbs; i++) {
		const VerbSlot &verb = _verbs[i];
		if (!verb.verbid || verb.saveid || !verb.curmode)
			continue;
		report.writeString(Common::String::format(
			"  slot=%d verb=%u rect=%d,%d,%d,%d key=%u\n",
			i, verb.verbid, verb.curRect.left, verb.curRect.top,
			verb.curRect.right, verb.curRect.bottom, verb.key));
	}

	report.finalize();
	debug(0, "Discovery report written: output/scummvm_discovery_v5_report.txt");
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
	startScene(roomId, nullptr, 0);
	initializeDiscoveryScriptVariables();
}

void Randomizer_v5::setupDiscoveryRoomObjects() {
	const byte *room = getResourceAddress(rtRoom, _roomResource);
	if (!room || _numObjectsInRoom == 0)
		return;

	ResourceIterator obcds(room, false);
	for (int i = 0; i < _numObjectsInRoom; i++) {
		const byte *obcd = obcds.findNext(MKTAG('O', 'B', 'C', 'D'));
		if (!obcd)
			return;

		ObjectData *od = &_objs[findLocalObjectSlot()];
		memset(od, 0, sizeof(*od));
		od->OBCDoffset = obcd - room;

		const CodeHeader *cdhd = (const CodeHeader *)findResourceData(MKTAG('C', 'D', 'H', 'D'), obcd);
		if (!cdhd)
			continue;

		od->obj_nr = READ_LE_UINT16(&(cdhd->v5.obj_id));
		od->x_pos = cdhd->v5.x * 8;
		od->y_pos = cdhd->v5.y * 8;
		od->width = cdhd->v5.w * 8;
		od->height = cdhd->v5.h * 8;
		od->flags = cdhd->v5.flags;
		od->parent = cdhd->v5.parent;
		od->walk_x = READ_LE_UINT16(&(cdhd->v5.walk_x));
		od->walk_y = READ_LE_UINT16(&(cdhd->v5.walk_y));
		od->actordir = cdhd->v5.actordir;
	}

	ResourceIterator obims(room, false);
	for (int i = 0; i < _numObjectsInRoom; i++) {
		const byte *obim = obims.findNext(MKTAG('O', 'B', 'I', 'M'));
		if (!obim)
			return;

		const ImageHeader *imhd = (const ImageHeader *)findResourceData(MKTAG('I', 'M', 'H', 'D'), obim);
		if (!imhd)
			continue;

		uint16 objId = READ_LE_UINT16(&(imhd->old.obj_id));
		for (int j = 1; j < _numLocalObjects; j++) {
			if (_objs[j].obj_nr == objId) {
				_objs[j].OBIMoffset = obim - room;
				break;
			}
		}
	}
}

void Randomizer_v5::executeScriptAtOffset(uint16 scriptNumber, uint32 scriptOffset, byte where, int *vars) {
	// Check safeguards to prevent runaway scripts during discovery
	if (_discoveryRecursionDepth >= MAX_DISCOVERY_RECURSION) {
		debug(1, "  Discovery: hit recursion limit, skipping script");
		return;
	}

	if (where == WIO_ROOM || where == WIO_LOCAL) {
		const byte *roomBase = getResourceAddress(rtRoom, _roomResource);
		if (!roomBase) {
			debug(2, "  Discovery: room %d not available for script offset %u", _roomResource, scriptOffset);
			return;
		}

		const uint32 roomSize = READ_BE_UINT32(roomBase + 4);
		if (roomSize < 8 || scriptOffset >= roomSize) {
			debug(2, "  Discovery: skipping out-of-range script offset %u (room %d size %u)", scriptOffset, _roomResource, roomSize);
			return;
		}
	}

	_discoveryRecursionDepth++;

	int slot = getScriptSlot();
	if (slot < 0 || slot >= NUM_SCRIPT_SLOT) {
		_discoveryRecursionDepth--;
		return;
	}

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

	// Run the script via base class method
	runScriptNested(slot);

	_discoveryRecursionDepth--;
}

void Randomizer_v5::o5_pickupObject() {
	debug(2, "  Discovery: intercepted pickupObject opcode");
	const byte *savedScriptPointer = _scriptPointer;
	const uint16 scriptOffset = (_currentScript != 0xFF && _scriptPointer && _scriptOrgPointer) ? (uint16)(_scriptPointer - _scriptOrgPointer) : 0;
	int obj = getVarOrDirectWord(PARAM_1);
	int room = getVarOrDirectByte(PARAM_2);
	if (room == 0)
		room = _roomResource;

	PickupCall call;
	call.objectId = obj;
	call.room = room;
	call.scriptNum = (_currentScript != 0xFF) ? vm.slot[_currentScript].number : 0;
	call.scriptOffset = scriptOffset;
	_pickupCalls.push_back(call);
	_scriptPointer = savedScriptPointer;
	ScummEngine_v5::o5_pickupObject();
}

void Randomizer_v5::initializeDiscoveryScriptVariables() {
	if (VAR_SENTENCE_SCRIPT == 0xFF || VAR(VAR_SENTENCE_SCRIPT) != 0)
		return;

	const byte *script = getResourceAddress(rtScript, 1);
	if (!script)
		return;

	int scriptSize = getResourceSize(rtScript, 1);
	for (int pos = 0; pos + 4 < scriptSize; pos++) {
		if (script[pos] != 0x1A)
			continue;
		if (READ_LE_UINT16(script + pos + 1) != VAR_SENTENCE_SCRIPT)
			continue;

		VAR(VAR_SENTENCE_SCRIPT) = READ_LE_UINT16(script + pos + 3);
		debug(0, "Initialized sentence script to %d from boot script", (int)VAR(VAR_SENTENCE_SCRIPT));
		return;
	}
}

void Randomizer_v5::drainDiscoveryScripts() {
	for (int cycle = 0; cycle < MAX_DISCOVERY_DRAIN_CYCLES; cycle++) {
		if (++_discoveryDrainCount > MAX_DISCOVERY_INSTRUCTIONS) {
			debug(0, "Discovery drain budget reached (%u)", MAX_DISCOVERY_INSTRUCTIONS);
			return;
		}

		bool hasRunnableScript = false;
		for (int i = 0; i < NUM_SCRIPT_SLOT; i++) {
			if (vm.slot[i].status == ssRunning) {
				hasRunnableScript = true;
				break;
			}
		}

		if (!hasRunnableScript && !_sentenceNum)
			break;

		runAllScripts();
		checkAndRunSentenceScript();
		scummLoop_handleDrawing();
		scummLoop_handleSound();
	}
}

void Randomizer_v5::runDiscoverySentence(int roomId, uint16 objectA, uint16 objectB, byte verbId) {
	if (_currentRoom != roomId)
		return;

	if (VAR_ACTIVE_VERB != 0xFF)
		VAR(VAR_ACTIVE_VERB) = verbId;
	if (VAR_ACTIVE_OBJECT1 != 0xFF)
		VAR(VAR_ACTIVE_OBJECT1) = objectA;
	if (VAR_ACTIVE_OBJECT2 != 0xFF)
		VAR(VAR_ACTIVE_OBJECT2) = objectB;
	if (VAR_VERB_ALLOWED != 0xFF)
		VAR(VAR_VERB_ALLOWED) = getVerbEntrypoint(objectA, verbId) != 0;

	debug(2, "  Discovery sentence: sentenceScript=%d verb=%d objectA=%d objectB=%d entry=%d",
		  VAR_SENTENCE_SCRIPT != 0xFF ? (int)VAR(VAR_SENTENCE_SCRIPT) : -1,
		  verbId, objectA, objectB, getVerbEntrypoint(objectA, verbId));

	doSentence(verbId, objectA, objectB);
	checkAndRunSentenceScript();
	drainDiscoveryScripts();
}

void Randomizer_v5::runDiscoveryObjectScript(int roomId, uint16 objId, byte verbId) {
	if (_currentRoom != roomId)
		return;

	int vars[NUM_SCRIPT_LOCAL];
	memset(vars, 0, sizeof(vars));
	vars[0] = 0;
	vars[1] = verbId;

	if (VAR_ACTIVE_VERB != 0xFF)
		VAR(VAR_ACTIVE_VERB) = verbId;
	if (VAR_ACTIVE_OBJECT1 != 0xFF)
		VAR(VAR_ACTIVE_OBJECT1) = objId;
	if (VAR_ACTIVE_OBJECT2 != 0xFF)
		VAR(VAR_ACTIVE_OBJECT2) = 0;
	if (VAR_VERB_ALLOWED != 0xFF)
		VAR(VAR_VERB_ALLOWED) = 1;
	if (VAR_ME != 0xFF)
		VAR(VAR_ME) = objId;

	const uint32 obcdOffset = getOBCDOffs(objId);
	const uint32 entryOffset = getVerbEntrypoint(objId, verbId);
	const byte *room = getResourceAddress(rtRoom, roomId);
	debug(2, "  Discovery object script: room=%d obj=%d verb=%d obcd=%u entry=%u total=%u bytes=%02x %02x %02x %02x",
		  roomId, objId, verbId, obcdOffset, entryOffset, obcdOffset + entryOffset,
		  room[obcdOffset + entryOffset], room[obcdOffset + entryOffset + 1],
		  room[obcdOffset + entryOffset + 2], room[obcdOffset + entryOffset + 3]);

	runObjectScript(objId, verbId, false, false, vars);
	drainDiscoveryScripts();
}

bool Randomizer_v5::runRoom108UserInput(DiscoveryTraceStep &outTrace) {
	if (_room108InputStage == kRoom108Complete || _game.id != GID_MONKEY2 || _currentRoom != 108)
		return false;
	if ((_game.platform == Common::kPlatformMacintosh && _bootParam == -7873) ||
		(_game.features & GF_ULTIMATE_TALKIE))
		return false;
	if (VAR_VERB_SCRIPT == 0xFF || VAR(VAR_VERB_SCRIPT) != 132)
		return false;
	if (_room108InputStage != kRoom108PressEnter) {
		for (int i = 0; i < MAX_DISCOVERY_WAIT_STEPS * 4 && _userPut <= 0; i++) {
			scummLoop(DISCOVERY_WAIT_QUANTUM);
		}
		if (_userPut <= 0 || VAR(VAR_VERB_SCRIPT) != 132)
			return false;
	}

	DiscoveryStateSnapshot parentState;
	snapshotDiscoveryState(parentState);
	Common::Event event;
	Common::String label;
	if (_room108InputStage == kRoom108PressEnter) {
		event.type = Common::EVENT_KEYDOWN;
		event.kbd = Common::KeyState(Common::KEYCODE_RETURN, Common::ASCII_RETURN);
		label = "room-108-enter";
		parseEvent(event);
		processInput();
		checkExecVerbs();
		drainDiscoveryScripts();
	} else if (_room108InputStage == kRoom108RecipeOnes) {
		event.type = Common::EVENT_KEYDOWN;
		event.kbd = Common::KeyState(Common::KEYCODE_1, '1');
		label = Common::String::format("room-108-recipe-one-%d", _room108RecipeInputs + 1);
		parseEvent(event);
		processInput();
		checkExecVerbs();
		drainDiscoveryScripts();
	} else {
		event.type = Common::EVENT_MOUSEMOVE;
		event.mouse.x = 28;
		event.mouse.y = 64;
		label = "room-108-select-full-game";
		parseEvent(event);
		event.type = Common::EVENT_LBUTTONDOWN;
		parseEvent(event);
		processInput();
		checkExecVerbs();
		drainDiscoveryScripts();
		event.type = Common::EVENT_LBUTTONUP;
		parseEvent(event);
	}

	DiscoveryStateSnapshot childState;
	snapshotDiscoveryState(childState);
	outTrace.parentHash = parentState.hash;
	outTrace.stateHash = childState.hash;
	outTrace.roomId = parentState.roomId;
	outTrace.objectA = 0;
	outTrace.objectB = 0;
	outTrace.egoObject = (VAR_EGO != 0xFF) ? VAR(VAR_EGO) : 0;
	outTrace.verbId = 0;
	outTrace.scriptNum = 128;
	outTrace.scriptOffset = 0;
	outTrace.actionType = DiscoveryAction::kUserInput;
	outTrace.actionLabel = label;
	_discoveryInputAttempts.push_back(outTrace);
	const bool recipeInput = _room108InputStage == kRoom108RecipeOnes;
	const bool stateChanged = parentState.hash != childState.hash;
	if (!stateChanged && !recipeInput)
		return false;

	if (_room108InputStage == kRoom108PressEnter) {
		_room108InputStage = kRoom108RecipeOnes;
	} else if (_room108InputStage == kRoom108RecipeOnes) {
		_room108RecipeInputs++;
		if (_room108RecipeInputs == 4)
			_room108InputStage = kRoom108SelectFullGame;
	} else {
		_room108InputStage = kRoom108Complete;
	}

	return stateChanged || recipeInput;
}

void Randomizer_v5::discoverReachableRoom(Common::Array<ObjCatalogEntry> &catalog, Common::HashMap<uint16, int> &objToCatalogIdx, int roomId) {
	if (roomId <= 0 || roomId >= _numRooms)
		return;

	_discoverySeenRooms[(uint16)roomId] = true;
	if (_currentRoom != roomId)
		return;

	const byte *roomPtr = getResourceAddress(rtRoom, roomId);
	if (!roomPtr)
		return;

	const byte *rmhd = findResourceData(MKTAG('R', 'M', 'H', 'D'), roomPtr);
	if (!rmhd)
		return;

	int numObjects = READ_LE_UINT16(&((const RoomHeader *)rmhd)->old.numObjects);
	checkpointDiscoveryState();

	for (int waitStep = 0; waitStep < MAX_DISCOVERY_WAIT_STEPS; waitStep++) {
		if (_currentRoom != roomId)
			return;

		if (waitStep > 0) {
			DiscoveryStateSnapshot parentState;
			snapshotDiscoveryState(parentState);
			decreaseScriptDelay(DISCOVERY_WAIT_QUANTUM);
			runAllScripts();
			checkAndRunSentenceScript();
			drainDiscoveryScripts();
			DiscoveryStateSnapshot childState;
			snapshotDiscoveryState(childState);
			checkpointDiscoveryState();
			DiscoveryTraceStep waitTrace;
			waitTrace.parentHash = parentState.hash;
			waitTrace.stateHash = childState.hash;
			waitTrace.roomId = roomId;
			waitTrace.objectA = 0;
			waitTrace.objectB = 0;
			waitTrace.egoObject = (VAR_EGO != 0xFF) ? VAR(VAR_EGO) : 0;
			waitTrace.verbId = 0;
			waitTrace.scriptNum = 0;
			waitTrace.scriptOffset = 0;
			waitTrace.actionType = DiscoveryAction::kWait;
			waitTrace.actionLabel = "wait-frame";
			recordDiscoveryTrace(waitTrace);
			if (!registerDiscoveryState(childState.hash))
				return;
			if (_currentRoom != roomId)
				return;
		}

		roomPtr = getResourceAddress(rtRoom, roomId);
		if (!roomPtr)
			return;
		rmhd = findResourceData(MKTAG('R', 'M', 'H', 'D'), roomPtr);
		if (!rmhd)
			return;
		numObjects = READ_LE_UINT16(&((const RoomHeader *)rmhd)->old.numObjects);

		Common::Array<uint16> objectIds;
		Common::Array<byte> verbIds;
		ResourceIterator obcds(roomPtr, false);
		for (int objectIndex = 0; objectIndex < numObjects; objectIndex++) {
			const byte *obcdPtr = obcds.findNext(MKTAG('O', 'B', 'C', 'D'));
			if (!obcdPtr)
				break;
			const uint32 obcdSize = READ_BE_UINT32(obcdPtr + 4);
			if (obcdSize < 8)
				continue;
			Common::Array<byte> cdhdBlock = extractBlock(obcdPtr, obcdSize, MKTAG('C', 'D', 'H', 'D'));
			if (cdhdBlock.size() < 10)
				continue;
			const uint16 objId = READ_LE_UINT16(cdhdBlock.data() + 8);
			if (!objToCatalogIdx.contains(objId)) {
				ObjCatalogEntry entry;
				entry.roomId = (byte)roomId;
				entry.objId = objId;
				entry.fullObcd.resize(obcdSize);
				memcpy(entry.fullObcd.data(), obcdPtr, obcdSize);
				entry.cdhdBlock = cdhdBlock;
				entry.verbBlock = extractBlock(obcdPtr, obcdSize, MKTAG('V', 'E', 'R', 'B'));
				entry.obnaBlock = extractBlock(obcdPtr, obcdSize, MKTAG('O', 'B', 'N', 'A'));
				entry.name = getObjName(entry.obnaBlock);
				objToCatalogIdx[objId] = catalog.size();
				catalog.push_back(entry);
			}
			const byte *verbBlock = findResourceData(MKTAG('V', 'E', 'R', 'B'), obcdPtr);
			if (!verbBlock)
				continue;
			const uint32 verbChunkSize = READ_BE_UINT32(verbBlock - 4);
			if (verbChunkSize < 8)
				continue;
			const byte *verbPtr = verbBlock;
			const byte *verbEnd = verbBlock + verbChunkSize - 8;
			while (verbPtr + 2 < verbEnd && *verbPtr != 0x00) {
				objectIds.push_back(objId);
				verbIds.push_back(*verbPtr);
				verbPtr += 3;
			}
		}

		for (uint actionIndex = 0; actionIndex < objectIds.size(); actionIndex++) {
			for (uint secondaryIndex = 0; secondaryIndex <= objectIds.size(); secondaryIndex++) {
				if (_currentRoom != roomId)
					return;
				const uint16 objectA = objectIds[actionIndex];
				const uint16 objectB = secondaryIndex < objectIds.size() ? objectIds[secondaryIndex] : 0;
				if (objectB == objectA)
					continue;
				const byte verbId = verbIds[actionIndex];
				DiscoveryStateSnapshot parentState;
				snapshotDiscoveryState(parentState);
				const uint pickupCount = _pickupCalls.size();
				runDiscoverySentence(roomId, objectA, objectB, verbId);
				DiscoveryStateSnapshot childState;
				snapshotDiscoveryState(childState);
				checkpointDiscoveryState();
				DiscoveryTraceStep sentenceTrace;
				sentenceTrace.parentHash = parentState.hash;
				sentenceTrace.stateHash = childState.hash;
				sentenceTrace.roomId = roomId;
				sentenceTrace.objectA = objectA;
				sentenceTrace.objectB = objectB;
				sentenceTrace.egoObject = (VAR_EGO != 0xFF) ? VAR(VAR_EGO) : 0;
				sentenceTrace.verbId = verbId;
				sentenceTrace.scriptNum = _pickupCalls.size() > pickupCount ? _pickupCalls.back().scriptNum : 0;
				sentenceTrace.scriptOffset = _pickupCalls.size() > pickupCount ? _pickupCalls.back().scriptOffset : 0;
				sentenceTrace.actionType = DiscoveryAction::kSentence;
				sentenceTrace.actionLabel = "sentence";
				recordDiscoveryTrace(sentenceTrace);
				if (!registerDiscoveryState(childState.hash))
					return;

				if (_currentRoom == roomId && objectB == 0 && _pickupCalls.size() == pickupCount && getVerbEntrypoint(objectA, verbId)) {
					parentState = childState;
					runDiscoveryObjectScript(roomId, objectA, verbId);
					snapshotDiscoveryState(childState);
					checkpointDiscoveryState();
					DiscoveryTraceStep scriptTrace;
					scriptTrace.parentHash = parentState.hash;
					scriptTrace.stateHash = childState.hash;
					scriptTrace.roomId = roomId;
					scriptTrace.objectA = objectA;
					scriptTrace.objectB = 0;
					scriptTrace.egoObject = (VAR_EGO != 0xFF) ? VAR(VAR_EGO) : 0;
					scriptTrace.verbId = verbId;
					scriptTrace.scriptNum = 0;
					scriptTrace.scriptOffset = 0;
					scriptTrace.actionType = DiscoveryAction::kObjectScript;
					scriptTrace.actionLabel = "object-script-fallback";
					recordDiscoveryTrace(scriptTrace);
					if (!registerDiscoveryState(childState.hash))
						return;
				}

				if (_currentRoom != roomId)
					return;
			}
		}

		if (waitStep == MAX_DISCOVERY_WAIT_STEPS - 1) {
			for (int inputStep = 0; inputStep < 6 && _currentRoom == roomId; inputStep++) {
				DiscoveryTraceStep inputTrace;
				if (!runRoom108UserInput(inputTrace))
					break;
			checkpointDiscoveryState();
				recordDiscoveryTrace(inputTrace);
				if (!registerDiscoveryState(inputTrace.stateHash))
					return;
			}
		}
	}
}

void Randomizer_v5::exploreReachableStateGraph(Common::Array<ObjCatalogEntry> &catalog, Common::Array<int> &outPickuppableIndices) {
	debug(0, "Discovering pickuppable objects via interpreter...");
	_pickupCalls.clear();
	_discoveryRecursionDepth = 0;
	_discoveryDrainCount = 0;

	Common::HashMap<uint16, int> objToCatalogIdx;
	int room108StagePasses = 0;
	// Dynamic discovery starts from the real game boot path. Object metadata is
	// added only while the live state explorer visits a reachable room.
	debug(0, "Executing boot script and traversing reachable rooms...");
	_discoveryMode = true;
	runBootscript();
	drainDiscoveryScripts();
	initializeDiscoveryScriptVariables();
	beginDiscoveryGraphSearch();
	while (_currentRoom > 0 && _discoverySeenStates.size() < MAX_DISCOVERY_STATES &&
		   _discoveryDrainCount < MAX_DISCOVERY_INSTRUCTIONS) {
		DiscoveryStateSnapshot beforeRoom;
		snapshotDiscoveryState(beforeRoom);
		Room108InputStage room108StageBefore = _room108InputStage;
		const int roomBefore = _currentRoom;
		discoverReachableRoom(catalog, objToCatalogIdx, roomBefore);
		DiscoveryStateSnapshot afterRoom;
		snapshotDiscoveryState(afterRoom);
		if (_currentRoom == 108 && _room108InputStage != kRoom108Complete &&
			room108StageBefore != _room108InputStage && room108StagePasses++ < 6)
			continue;
		if (_currentRoom == roomBefore || beforeRoom.hash == afterRoom.hash)
			break;
	}
	_discoveryMode = false;
	debug(0, "Interpreter probes captured %d pickup calls across %d reachable rooms", _pickupCalls.size(), _discoverySeenRooms.size());

	// Now mark which objects are pickuppable based on collected calls
	// (accumulated across all executed scripts)
	Common::HashMap<uint16, bool> pickuppableObjs;
	for (int i = 0; i < (int)_pickupCalls.size(); i++) {
		pickuppableObjs[_pickupCalls[i].objectId] = true;
		debug(1, "  Found pickup call: obj %d in room %d", _pickupCalls[i].objectId, _pickupCalls[i].room);
	}

	for (int i = 0; i < (int)catalog.size(); i++) {
		if (pickuppableObjs.contains(catalog[i].objId)) {
			catalog[i].isPickuppable = true;
			debug(1, "  Marked obj %d as pickuppable (interpreter discovery)", catalog[i].objId);
		}
	}

	// Build list of pickuppable indices
	for (int i = 0; i < (int)catalog.size(); i++) {
		if (catalog[i].isPickuppable)
			outPickuppableIndices.push_back(i);
	}

	debug(0, "Discovery complete: %d pickuppable objects found", outPickuppableIndices.size());
	writeDiscoveryReport(catalog, outPickuppableIndices);
}

Common::Error Randomizer_v5::run() {
	Common::Error err;
	err = init();
	if (err.getCode() != Common::kNoError)
		return err;
	setTotalPlayTime();
	_lastWaitTime = _system->getMillis();
	return randomize();
}

Common::Error Randomizer_v5::randomize() {
	debug(0, "=== SCUMM V5 OBCD Swap Randomizer ===");

	Common::Array<ObjCatalogEntry> catalog;
	Common::Array<int> pickuppableIndices;

	// Use live interpreter-based state exploration instead of static analysis.
	exploreReachableStateGraph(catalog, pickuppableIndices);

	debug(0, "Total objects: %d, pick-uppable: %d", catalog.size(), pickuppableIndices.size());
	for (int i = 0; i < (int)pickuppableIndices.size(); i++) {
		const ObjCatalogEntry &entry = catalog[pickuppableIndices[i]];
		debug(0, "  Pickuppable: obj %d in room %d: \"%s\"", entry.objId, entry.roomId, entry.name.c_str());
	}

	if (pickuppableIndices.size() < 2) {
		warning("randomizer: Not enough pickuppable objects to shuffle");
		return Common::kNoError;
	}

	// Build mapping from objId to catalog index for later use
	Common::HashMap<uint16, int> objToCatalogIdx;
	for (int i = 0; i < (int)catalog.size(); i++) {
		objToCatalogIdx[catalog[i].objId] = i;
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