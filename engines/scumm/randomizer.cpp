#include "scumm/scumm_v5.h"
#include "scumm/resource.h"
#include "scumm/object.h"
#include "common/array.h"
#include "common/random.h"
#include "common/file.h"
#include "common/memstream.h"
#include "common/endian.h"
#include "common/system.h"

using namespace Scumm;

// ============================================================================
// Structures
// ============================================================================

// A catalog entry for one pick-uppable object found in a room.
struct ObcdCatalogEntry {
	uint16 objId;       // from CDHD.v5.obj_id
	int roomNo;         // which room index this object lives in

	// Raw CDHD block (including 8-byte header tag+size)
	Common::Array<byte> cdhdBlock;
	// Raw OBNA block (including 8-byte header)
	Common::Array<byte> obnaBlock;
	// Raw VERB block (including 8-byte header)
	Common::Array<byte> verbBlock;
	// The full original OBCD block
	Common::Array<byte> obcdBlock;

	// Pickup verb script bytes extracted from the verb block.
	// Empty if no pickup verb exists.
	Common::Array<byte> pickupVerbScript;

	// The verb ID that contains the pickupObject opcode
	byte pickupVerbId;

	bool hasPickupVerb;
};

// ============================================================================
// Helpers: Block reading
// ============================================================================

static uint32 readBE32(const byte *p) { return READ_BE_UINT32(p); }
static uint32 readLE32(const byte *p) { return READ_LE_UINT32(p); }
static uint16 readLE16(const byte *p) { return READ_LE_UINT16(p); }

static void writeBE32(byte *p, uint32 v) { WRITE_BE_UINT32(p, v); }
static void writeLE16(byte *p, uint16 v) { WRITE_LE_UINT16(p, v); }
static void writeLE32(byte *p, uint32 v) { WRITE_LE_UINT32(p, v); }

// Get the 4-char tag at a block pointer (big endian)
static uint32 blockTag(const byte *p) { return readBE32(p); }
// Get the block size (including 8-byte header)
static uint32 blockSize(const byte *p) { return readBE32(p + 4); }

// Copy a raw block (tag+size+data) into a byte array 
static Common::Array<byte> copyBlock(const byte *p) {
	uint32 sz = blockSize(p);
	Common::Array<byte> out(sz);
	memcpy(out.data(), p, sz);
	return out;
}

// Find a sub-block by tag within a parent block. Returns pointer to the sub-block
// start (tag+size) or nullptr. Scans immediate children only.
static const byte *findSubBlock(const byte *parent, uint32 tag) {
	uint32 parentSize = blockSize(parent);
	const byte *cur = parent + 8; // skip parent header
	const byte *end = parent + parentSize;
	while (cur + 8 <= end) {
		uint32 t = blockTag(cur);
		uint32 s = blockSize(cur);
		if (s < 8 || cur + s > end)
			break;
		if (t == tag)
			return cur;
		cur += s;
	}
	return nullptr;
}

// ============================================================================
// Helpers: VERB block parsing for V5
// ============================================================================

// The VERB block (after 8-byte header) has an entry table:
//   { verb_id(1 byte), offset(2 bytes LE) } repeated
//   terminated by verb_id == 0x00
//
// The offsets are relative to the start of the VERB block (including header).

struct VerbEntry {
	byte verbId;
	uint16 offset; // relative to VERB block start (including header)
};

// Parse the verb entry table from a VERB block
static Common::Array<VerbEntry> parseVerbEntries(const byte *verbBlock) {
	Common::Array<VerbEntry> entries;
	const byte *p = verbBlock + 8; // skip header
	while (*p != 0x00) {
		VerbEntry e;
		e.verbId = *p;
		e.offset = readLE16(p + 1);
		entries.push_back(e);
		p += 3;
	}
	return entries;
}

// Extract the script bytes for a specific verb from the VERB block (returns empty if not found)
static Common::Array<byte> extractVerbScript(const byte *verbBlock, uint32 verbBlockSize, byte targetVerbId) {
	Common::Array<VerbEntry> entries = parseVerbEntries(verbBlock);

	int targetIdx = -1;
	for (int i = 0; i < (int)entries.size(); i++) {
		if (entries[i].verbId == targetVerbId) {
			targetIdx = i;
			break;
		}
	}
	if (targetIdx < 0)
		return Common::Array<byte>();

	uint16 startOff = entries[targetIdx].offset;

	// Find the end: it's the start of the next verb script that begins after this one,
	// or the end of the VERB block
	uint32 endOff = verbBlockSize;
	for (int i = 0; i < (int)entries.size(); i++) {
		if (entries[i].offset > startOff && entries[i].offset < endOff)
			endOff = entries[i].offset;
	}

	if (startOff >= verbBlockSize || endOff > verbBlockSize)
		return Common::Array<byte>();

	uint32 len = endOff - startOff;
	Common::Array<byte> script(len);
	memcpy(script.data(), verbBlock + startOff, len);
	return script;
}

// Check if a verb script contains a pickupObject opcode (0x25, 0x65, 0xA5, 0xE5).
// This is a simple byte scan — not a full opcode walk — looking for the
// pickupObject base opcode byte. Since script bytes can appear as parameters,
// this may have false positives, but in practice it's reliable enough for
// identifying which verb scripts call pickupObject.
static bool verbScriptContainsPickup(const byte *script, uint32 len) {
	for (uint32 i = 0; i < len; i++) {
		byte b = script[i];
		if (b == 0x25 || b == 0x65 || b == 0xA5 || b == 0xE5)
			return true;
	}
	return false;
}

// Find which verb entry contains a pickupObject opcode. Returns the verb ID,
// or 0x00 if none found.
static byte findPickupVerbId(const byte *verbBlock, uint32 verbBlockSize) {
	Common::Array<VerbEntry> entries = parseVerbEntries(verbBlock);

	for (int i = 0; i < (int)entries.size(); i++) {
		uint16 startOff = entries[i].offset;
		uint32 endOff = verbBlockSize;
		for (int j = 0; j < (int)entries.size(); j++) {
			if (entries[j].offset > startOff && entries[j].offset < endOff)
				endOff = entries[j].offset;
		}
		if (startOff >= verbBlockSize || endOff > verbBlockSize)
			continue;
		uint32 len = endOff - startOff;
		if (verbScriptContainsPickup(verbBlock + startOff, len))
			return entries[i].verbId;
	}
	return 0x00;
}

// ============================================================================
// Step 3: Opcode-aware bytecode patcher (incremental)
// ============================================================================

// Parameter kind: how a parameter is read from the bytecode stream
enum ParamKind {
	PK_VARORBYTE,   // getVarOrDirectByte(PARAM_x) — mask-dependent: 1 byte literal or 2-byte var
	PK_VARORWORD,   // getVarOrDirectWord(PARAM_x)  — mask-dependent: 2 byte literal or 2-byte var
	PK_RESULTPOS,   // getResultPos() — always 2 bytes (fetchScriptWord), possibly +2 more for array index
	PK_JUMP,        // jumpRelative — always 2 bytes (signed offset)
	PK_BYTE,        // fetchScriptByte — always 1 byte
	PK_WORD,        // fetchScriptWord — always 2 bytes (signed)
};

// Which PARAM_x mask does this parameter use for var-or-direct opcodes?
// PARAM_1=0x80, PARAM_2=0x40, PARAM_3=0x20
static const byte PARAM_MASKS[] = { 0x80, 0x40, 0x20 };

// Flags for which parameters are object IDs to patch
enum ObjIdFlag {
	OBJ_ID_NONE = 0,
	OBJ_ID_P1 = 1,
	OBJ_ID_P2 = 2,
	OBJ_ID_P3 = 4,
};

// Parameter sizing: how many bytes does one param consume at a given script offset?
// For PK_VARORBYTE:  if (opcode & mask) → var ref → 2 bytes; else → 1 byte literal
// For PK_VARORWORD:  if (opcode & mask) → var ref → 2 bytes; else → 2 byte literal
// For PK_RESULTPOS:  2 bytes (fetchScriptWord), then possibly +2 if bit 0x2000 set
// For PK_JUMP:       2 bytes always
// For PK_BYTE:       1 byte always
// For PK_WORD:       2 bytes always

static int computeResultPosSize(const byte *script, int offset, int scriptLen) {
	if (offset + 2 > scriptLen)
		return -1;
	uint16 var = readLE16(script + offset);
	int consumed = 2;
	if (var & 0x2000) {
		if (offset + 4 > scriptLen)
			return -1;
		consumed = 4;
		// Could recurse further but in practice for v5 it's just +2
	}
	return consumed;
}

struct FullOpcodeEntry {
	ParamKind params[5];
	int numParams;
	int objIdFlags;  // bits: which param indices contain obj IDs (PK_VARORWORD only)
	bool isComplex;
};

static Common::Array<FullOpcodeEntry> buildFullOpcodeTable() {
	Common::Array<FullOpcodeEntry> table;
	table.resize(256);
	for (int i = 0; i < 256; i++) {
		table[i].numParams = 0;
		table[i].objIdFlags = OBJ_ID_NONE;
		table[i].isComplex = true; // default unknown = complex (skip)
	}

	// Helper lambda-like macros
	#define F_SIG0(op) do { table[op].numParams = 0; table[op].isComplex = false; table[op].objIdFlags = 0; } while(0)
	#define F_SIG(op, np, p0, p1, p2, p3, objf) do { \
		table[op].numParams = np; \
		table[op].params[0] = p0; table[op].params[1] = p1; \
		table[op].params[2] = p2; table[op].params[3] = p3; \
		table[op].objIdFlags = objf; \
		table[op].isComplex = false; \
	} while(0)
	#define F_SIG1(op, p0, objf) F_SIG(op, 1, p0, PK_BYTE, PK_BYTE, PK_BYTE, objf)
	#define F_SIG2(op, p0, p1, objf) F_SIG(op, 2, p0, p1, PK_BYTE, PK_BYTE, objf)
	#define F_SIG3(op, p0, p1, p2, objf) F_SIG(op, 3, p0, p1, p2, PK_BYTE, objf)
	#define F_SIG4(op, p0, p1, p2, p3, objf) F_SIG(op, 4, p0, p1, p2, p3, objf)

	// Apply a mirror pattern: for each opcode o in 0x00-0x7F, also define o+0x80
	// as the same signature (unless overridden after)
	// We'll just define both explicitly for the cases that mirror, and override the rest.

	// --- Pair handlers (same handler at base and base+0x80) ---

	// putActor: 0x01/0x21/0x41/0x61/0x81/0xA1/0xC1/0xE1
	for (int b : {0x01, 0x21, 0x41, 0x61, 0x81, 0xA1, 0xC1, 0xE1})
		F_SIG3(b, PK_VARORBYTE, PK_VARORWORD, PK_VARORWORD, OBJ_ID_NONE);

	// startMusic: 0x02/0x82
	for (int b : {0x02, 0x82})
		F_SIG1(b, PK_VARORBYTE, OBJ_ID_NONE);

	// getActorRoom: 0x03/0x83
	for (int b : {0x03, 0x83})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// isGreaterEqual: 0x04/0x84
	for (int b : {0x04, 0x84})
		F_SIG3(b, PK_WORD, PK_VARORWORD, PK_JUMP, OBJ_ID_NONE);

	// drawObject: 0x05/0x85 — complex (sub-opcode after first param)
	// (leave as complex)

	// getActorElevation: 0x06/0x86
	for (int b : {0x06, 0x86})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// setState: 0x07/0x47/0x87/0xC7
	for (int b : {0x07, 0x47, 0x87, 0xC7})
		F_SIG2(b, PK_VARORWORD, PK_VARORBYTE, OBJ_ID_P1);

	// isNotEqual: 0x08/0x88
	for (int b : {0x08, 0x88})
		F_SIG3(b, PK_WORD, PK_VARORWORD, PK_JUMP, OBJ_ID_NONE);

	// faceActor: 0x09/0x49/0x89/0xC9
	for (int b : {0x09, 0x49, 0x89, 0xC9})
		F_SIG2(b, PK_VARORBYTE, PK_VARORWORD, OBJ_ID_P2);

	// startScript: 0x0a/0x2a/0x4a/0x6a/0x8a/0xAa/0xCa/0xEa — complex (vararg)
	// (leave as complex)

	// getVerbEntrypoint: 0x0b/0x4b/0x8b/0xCb
	for (int b : {0x0b, 0x4b, 0x8b, 0xCb})
		F_SIG3(b, PK_RESULTPOS, PK_VARORWORD, PK_VARORWORD, OBJ_ID_P2);

	// resourceRoutines: 0x0c/0x8c — complex
	// walkActorToActor: 0x0d/0x4d/0x8d/0xCd
	for (int b : {0x0d, 0x4d, 0x8d, 0xCd})
		F_SIG3(b, PK_VARORBYTE, PK_VARORBYTE, PK_BYTE, OBJ_ID_NONE);

	// putActorAtObject: 0x0e/0x4e/0x8e/0xCe
	for (int b : {0x0e, 0x4e, 0x8e, 0xCe})
		F_SIG2(b, PK_VARORBYTE, PK_VARORWORD, OBJ_ID_P2);

	// getObjectState: 0x0f/0x8f
	for (int b : {0x0f, 0x8f})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_P1);

	// getObjectOwner: 0x10/0x90
	for (int b : {0x10, 0x90})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_P1);

	// animateActor: 0x11/0x51/0x91/0xD1
	for (int b : {0x11, 0x51, 0x91, 0xD1})
		F_SIG2(b, PK_VARORBYTE, PK_VARORBYTE, OBJ_ID_NONE);

	// panCameraTo: 0x12/0x92
	for (int b : {0x12, 0x92})
		F_SIG1(b, PK_VARORWORD, OBJ_ID_NONE);

	// actorOps: 0x13/0x53/0x93/0xD3 — complex
	// print: 0x14/0x94 — complex
	// actorFromPos: 0x15/0x55/0x95/0xD5
	for (int b : {0x15, 0x55, 0x95, 0xD5})
		F_SIG3(b, PK_RESULTPOS, PK_VARORWORD, PK_VARORWORD, OBJ_ID_NONE);

	// getRandomNr: 0x16/0x96
	for (int b : {0x16, 0x96})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// and: 0x17/0x97
	for (int b : {0x17, 0x97})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// jumpRelative: 0x18
	F_SIG1(0x18, PK_JUMP, OBJ_ID_NONE);

	// systemOps: 0x98 — reads a sub-opcode byte (complex for safety)
	table[0x98].isComplex = true;

	// doSentence: 0x19/0x39/0x59/0x79/0x99/0xB9/0xD9/0xF9 — complex (conditional reads)
	// move: 0x1a/0x9a
	for (int b : {0x1a, 0x9a})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// multiply: 0x1b/0x9b
	for (int b : {0x1b, 0x9b})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// startSound: 0x1c/0x9c
	for (int b : {0x1c, 0x9c})
		F_SIG1(b, PK_VARORBYTE, OBJ_ID_NONE);

	// ifClassOfIs: 0x1d/0x9d — complex (vararg)
	// walkActorTo: 0x1e/0x3e/0x5e/0x7e/0x9e/0xBe/0xDe/0xFe
	for (int b : {0x1e, 0x3e, 0x5e, 0x7e, 0x9e, 0xBe, 0xDe, 0xFe})
		F_SIG3(b, PK_VARORBYTE, PK_VARORWORD, PK_VARORWORD, OBJ_ID_NONE);

	// isActorInBox: 0x1f/0x5f/0x9f/0xDf
	for (int b : {0x1f, 0x5f, 0x9f, 0xDf})
		F_SIG3(b, PK_VARORBYTE, PK_VARORBYTE, PK_JUMP, OBJ_ID_NONE);

	// stopMusic: 0x20
	F_SIG0(0x20);

	// getAnimCounter: 0x22/0xA2
	for (int b : {0x22, 0xA2})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// getActorY: 0x23/0xA3
	for (int b : {0x23, 0xA3})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// loadRoomWithEgo: 0x24/0x64/0xA4/0xE4
	for (int b : {0x24, 0x64, 0xA4, 0xE4})
		F_SIG4(b, PK_VARORWORD, PK_VARORBYTE, PK_WORD, PK_WORD, OBJ_ID_P1);

	// pickupObject: 0x25/0x65/0xA5/0xE5
	for (int b : {0x25, 0x65, 0xA5, 0xE5})
		F_SIG2(b, PK_VARORWORD, PK_VARORBYTE, OBJ_ID_P1);

	// setVarRange: 0x26/0xA6 — complex
	// stringOps: 0x27 — complex, 0xA7 = dummy (no params)
	F_SIG0(0xA7);

	// equalZero: 0x28
	F_SIG2(0x28, PK_WORD, PK_JUMP, OBJ_ID_NONE);

	// notEqualZero: 0xA8
	F_SIG2(0xA8, PK_WORD, PK_JUMP, OBJ_ID_NONE);

	// setOwnerOf: 0x29/0x69/0xA9/0xE9
	for (int b : {0x29, 0x69, 0xA9, 0xE9})
		F_SIG2(b, PK_VARORWORD, PK_VARORBYTE, OBJ_ID_P1);

	// saveRestoreVerbs: 0xAB — reads sub-opcode byte, then 3 bytes (complex pattern)
	// Actually: fetchScriptByte (sub-op), then byte, byte, byte
	F_SIG4(0xAB, PK_BYTE, PK_VARORBYTE, PK_VARORBYTE, PK_VARORBYTE, OBJ_ID_NONE);

	// delayVariable: 0x2b
	F_SIG1(0x2b, PK_WORD, OBJ_ID_NONE);

	// cursorCommand: 0x2c/0xAC — wait, 0xAC is expression
	// expression: 0xAC — complex (recursive opcode loop)
	table[0xAC].isComplex = true;

	// putActorInRoom: 0x2d/0x6d/0xAd/0xEd
	for (int b : {0x2d, 0x6d, 0xAd, 0xEd})
		F_SIG2(b, PK_VARORBYTE, PK_VARORBYTE, OBJ_ID_NONE);

	// delay: 0x2e — 3 literal bytes
	F_SIG3(0x2e, PK_BYTE, PK_BYTE, PK_BYTE, OBJ_ID_NONE);

	// wait: 0xAE — complex (sub-opcode)
	table[0xAE].isComplex = true;

	// matrixOps: 0x30/0xB0 — complex
	// getInventoryCount: 0x31/0xB1
	for (int b : {0x31, 0xB1})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// setCameraAt: 0x32/0xB2
	for (int b : {0x32, 0xB2})
		F_SIG1(b, PK_VARORWORD, OBJ_ID_NONE);

	// roomOps: 0x33/0x73/0xB3/0xF3 — complex
	// getDist: 0x34/0x74/0xB4/0xF4
	for (int b : {0x34, 0x74, 0xB4, 0xF4})
		F_SIG3(b, PK_RESULTPOS, PK_VARORWORD, PK_VARORWORD, OBJ_ID_NONE);

	// findObject: 0x35/0x75/0xB5/0xF5
	for (int b : {0x35, 0x75, 0xB5, 0xF5})
		F_SIG3(b, PK_RESULTPOS, PK_VARORWORD, PK_VARORWORD, OBJ_ID_NONE);

	// walkActorToObject: 0x36/0x76/0xB6/0xF6
	for (int b : {0x36, 0x76, 0xB6, 0xF6})
		F_SIG2(b, PK_VARORBYTE, PK_VARORWORD, OBJ_ID_P2);

	// startObject: 0x37/0x77/0xB7/0xF7 — complex (vararg)
	// isLessEqual: 0x38/0xB8
	for (int b : {0x38, 0xB8})
		F_SIG3(b, PK_WORD, PK_VARORWORD, PK_JUMP, OBJ_ID_NONE);

	// subtract: 0x3a/0xBa
	for (int b : {0x3a, 0xBa})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// getActorScale: 0x3b/0xBb
	for (int b : {0x3b, 0xBb})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// stopSound: 0x3c/0xBc
	for (int b : {0x3c, 0xBc})
		F_SIG1(b, PK_VARORBYTE, OBJ_ID_NONE);

	// findInventory: 0x3d/0x7d/0xBd/0xFd
	for (int b : {0x3d, 0x7d, 0xBd, 0xFd})
		F_SIG3(b, PK_RESULTPOS, PK_VARORBYTE, PK_VARORBYTE, OBJ_ID_NONE);

	// drawBox: 0x3f/0xBf/0x7f/0xFf — complex

	// cutscene: 0x40 — complex (vararg)
	// endCutscene: 0xC0 — no params
	F_SIG0(0xC0);

	// chainScript: 0x42/0xC2 — complex (vararg)
	// getActorX: 0x43/0xC3
	for (int b : {0x43, 0xC3})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// isLess: 0x44/0xC4
	for (int b : {0x44, 0xC4})
		F_SIG3(b, PK_WORD, PK_VARORWORD, PK_JUMP, OBJ_ID_NONE);

	// increment: 0x46
	F_SIG1(0x46, PK_RESULTPOS, OBJ_ID_NONE);
	// decrement: 0xC6
	F_SIG1(0xC6, PK_RESULTPOS, OBJ_ID_NONE);

	// isEqual: 0x48/0xC8
	for (int b : {0x48, 0xC8})
		F_SIG3(b, PK_WORD, PK_VARORWORD, PK_JUMP, OBJ_ID_NONE);

	// soundKludge: 0x4c — complex (vararg)

	// pseudoRoom: 0xCC — complex (loop reading bytes until 0)
	table[0xCC].isComplex = true;

	// actorFollowCamera: 0x52/0xD2
	for (int b : {0x52, 0xD2})
		F_SIG1(b, PK_VARORBYTE, OBJ_ID_NONE);

	// setObjectName: 0x54/0xD4 — complex (inline string)

	// getActorMoving: 0x56/0xD6
	for (int b : {0x56, 0xD6})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// or: 0x57/0xD7
	for (int b : {0x57, 0xD7})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// beginOverride: 0x58 — fetchScriptByte, then if != 0: fetchScriptByte + fetchScriptWord
	// It's effectively: byte + byte + word, but only if the first byte != 0.
	// Mark complex for safety.
	table[0x58].isComplex = true;

	// printEgo: 0xD8 — complex (decodeParseString)
	table[0xD8].isComplex = true;

	// add: 0x5a/0xDa
	for (int b : {0x5a, 0xDa})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// divide: 0x5b/0xDb
	for (int b : {0x5b, 0xDb})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// setClass: 0x5d/0xDd — complex (vararg)

	// freezeScripts: 0x60/0xE0
	for (int b : {0x60, 0xE0})
		F_SIG1(b, PK_VARORBYTE, OBJ_ID_NONE);

	// stopScript: 0x62/0xE2
	for (int b : {0x62, 0xE2})
		F_SIG1(b, PK_VARORBYTE, OBJ_ID_NONE);

	// getActorFacing: 0x63/0xE3
	for (int b : {0x63, 0xE3})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// getClosestObjActor: 0x66/0xE6
	for (int b : {0x66, 0xE6})
		F_SIG2(b, PK_RESULTPOS, PK_VARORWORD, OBJ_ID_NONE);

	// getStringWidth: 0x67/0xE7
	for (int b : {0x67, 0xE7})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// isScriptRunning: 0x68/0xE8
	for (int b : {0x68, 0xE8})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// debug: 0x6b/0xEb
	for (int b : {0x6b, 0xEb})
		F_SIG1(b, PK_VARORWORD, OBJ_ID_NONE);

	// getActorWidth: 0x6c/0xEc
	for (int b : {0x6c, 0xEc})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// stopObjectScript: 0x6e/0xEe
	for (int b : {0x6e, 0xEe})
		F_SIG1(b, PK_VARORWORD, OBJ_ID_P1);

	// lights: 0x70/0xF0
	for (int b : {0x70, 0xF0})
		F_SIG3(b, PK_VARORBYTE, PK_BYTE, PK_BYTE, OBJ_ID_NONE);

	// getActorCostume: 0x71/0xF1
	for (int b : {0x71, 0xF1})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// loadRoom: 0x72/0xF2
	for (int b : {0x72, 0xF2})
		F_SIG1(b, PK_VARORBYTE, OBJ_ID_NONE);

	// isGreater: 0x78/0xF8
	for (int b : {0x78, 0xF8})
		F_SIG3(b, PK_WORD, PK_VARORWORD, PK_JUMP, OBJ_ID_NONE);

	// verbOps: 0x7a/0xFa — complex
	// getActorWalkBox: 0x7b/0xFb
	for (int b : {0x7b, 0xFb})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// isSoundRunning: 0x7c/0xFc
	for (int b : {0x7c, 0xFc})
		F_SIG2(b, PK_RESULTPOS, PK_VARORBYTE, OBJ_ID_NONE);

	// breakHere: 0x80 — no params
	F_SIG0(0x80);

	// stopObjectCode: 0x00/0xA0
	F_SIG0(0x00);
	F_SIG0(0xA0);

	#undef F_SIG
	#undef F_SIG0
	#undef F_SIG1
	#undef F_SIG2
	#undef F_SIG3
	#undef F_SIG4

	return table;
}

// Walk the bytecode, patching direct-word obj IDs from donorObjId to slotObjId.
// Returns true if the walk completed without hitting an unknown opcode.
// Returns false if a complex/unknown opcode was encountered (bytecode is left partially patched).
static bool patchVerbBytecode(byte *script, int scriptLen, uint16 donorObjId, uint16 slotObjId) {
	static Common::Array<FullOpcodeEntry> opcodeTable = buildFullOpcodeTable();

	int pos = 0;
	while (pos < scriptLen) {
		byte opcode = script[pos];
		pos++;

		const FullOpcodeEntry &entry = opcodeTable[opcode];
		if (entry.isComplex) {
			// Can't walk further — bail out
			warning("Randomizer: complex opcode 0x%02X at offset %d, skipping rest of verb script", opcode, pos - 1);
			return false;
		}

		for (int p = 0; p < entry.numParams; p++) {
			if (pos >= scriptLen)
				return false;

			ParamKind pk = entry.params[p];
			byte paramMask = (p < 3) ? PARAM_MASKS[p] : 0;
			bool isVar = (pk == PK_VARORBYTE || pk == PK_VARORWORD) && (opcode & paramMask);

			switch (pk) {
			case PK_VARORBYTE:
				if (isVar) {
					pos += 2; // variable reference
				} else {
					pos += 1; // literal byte
				}
				break;

			case PK_VARORWORD:
				if (isVar) {
					pos += 2; // variable reference
				} else {
					// Direct word literal — check if this is an object ID position
					if ((entry.objIdFlags & (1 << p)) && pos + 1 < scriptLen) {
						uint16 val = readLE16(script + pos);
						if (val == donorObjId) {
							writeLE16(script + pos, slotObjId);
						}
					}
					pos += 2;
				}
				break;

			case PK_RESULTPOS: {
				int sz = computeResultPosSize(script, pos, scriptLen);
				if (sz < 0)
					return false;
				pos += sz;
				break;
			}

			case PK_JUMP:
				pos += 2;
				break;

			case PK_BYTE:
				pos += 1;
				break;

			case PK_WORD:
				pos += 2;
				break;
			}
		}
	}
	return true;
}

// ============================================================================
// Step 4: Build a merged VERB block
// ============================================================================

// Build a new VERB block that:
// - Uses donor's verb entries and scripts for all verbs EXCEPT 0x09
// - Uses slot's 0x09 script
// - Patches donor's non-0x09 scripts to replace donorObjId with slotObjId
static Common::Array<byte> buildMergedVerbBlock(
	const Common::Array<byte> &donorVerbBlock,
	const Common::Array<byte> &slotVerbBlock,
	uint16 donorObjId, uint16 slotObjId)
{
	// Parse both verb entry tables
	Common::Array<VerbEntry> donorEntries = parseVerbEntries(donorVerbBlock.data());
	Common::Array<VerbEntry> slotEntries = parseVerbEntries(slotVerbBlock.data());

	// Find 0x09 in both
	int slotV14Idx = -1;
	for (int i = 0; i < (int)slotEntries.size(); i++)
		if (slotEntries[i].verbId == 0x09) slotV14Idx = i;

	// Collect verb scripts to include
	struct VerbScript {
		byte verbId;
		Common::Array<byte> scriptData;
	};
	Common::Array<VerbScript> scripts;

	// Get donor scripts (except 0x09)
	for (int i = 0; i < (int)donorEntries.size(); i++) {
		if (donorEntries[i].verbId == 0x09)
			continue;

		uint16 startOff = donorEntries[i].offset;
		uint32 endOff = donorVerbBlock.size();
		for (int j = 0; j < (int)donorEntries.size(); j++) {
			if (donorEntries[j].offset > startOff && donorEntries[j].offset < endOff)
				endOff = donorEntries[j].offset;
		}

		VerbScript vs;
		vs.verbId = donorEntries[i].verbId;
		uint32 len = endOff - startOff;
		vs.scriptData.resize(len);
		memcpy(vs.scriptData.data(), donorVerbBlock.data() + startOff, len);

		// Patch the script
		patchVerbBytecode(vs.scriptData.data(), vs.scriptData.size(), donorObjId, slotObjId);

		scripts.push_back(vs);
	}

	// Get slot's 0x09 script
	if (slotV14Idx >= 0) {
		uint16 startOff = slotEntries[slotV14Idx].offset;
		uint32 endOff = slotVerbBlock.size();
		for (int j = 0; j < (int)slotEntries.size(); j++) {
			if (slotEntries[j].offset > startOff && slotEntries[j].offset < endOff)
				endOff = slotEntries[j].offset;
		}

		VerbScript vs;
		vs.verbId = 0x09;
		uint32 len = endOff - startOff;
		vs.scriptData.resize(len);
		memcpy(vs.scriptData.data(), slotVerbBlock.data() + startOff, len);
		scripts.push_back(vs);
	}

	// Assemble new VERB block:
	// Header: 4 bytes tag + 4 bytes size
	// Entry table: { verb_id(1), offset(2) } per entry, then 0x00 terminator
	// Then all script data
	uint32 entryTableSize = scripts.size() * 3 + 1; // +1 for terminator
	uint32 headerAndTable = 8 + entryTableSize;

	// Calculate script data offset
	uint32 totalScriptData = 0;
	for (const auto &vs : scripts)
		totalScriptData += vs.scriptData.size();

	uint32 totalSize = headerAndTable + totalScriptData;

	Common::Array<byte> result(totalSize);
	byte *out = result.data();

	// Write VERB tag
	writeBE32(out, MKTAG('V','E','R','B'));
	writeBE32(out + 4, totalSize);

	// Write entry table
	byte *entryPtr = out + 8;
	uint16 scriptOffset = headerAndTable;
	for (const auto &vs : scripts) {
		*entryPtr++ = vs.verbId;
		writeLE16(entryPtr, scriptOffset);
		entryPtr += 2;
		scriptOffset += vs.scriptData.size();
	}
	*entryPtr++ = 0x00; // terminator

	// Write script data
	byte *scriptPtr = out + headerAndTable;
	for (const auto &vs : scripts) {
		memcpy(scriptPtr, vs.scriptData.data(), vs.scriptData.size());
		scriptPtr += vs.scriptData.size();
	}

	return result;
}

// ============================================================================
// Step 4 (cont): Assemble a merged OBCD block
// ============================================================================

// Build a new OBCD = slot's CDHD + merged VERB + donor's OBNA
static Common::Array<byte> buildMergedObcd(
	const ObcdCatalogEntry &slot,
	const ObcdCatalogEntry &donor)
{
	Common::Array<byte> mergedVerb = buildMergedVerbBlock(
		donor.verbBlock, slot.verbBlock, donor.objId, slot.objId);

	// Total size = 8 (OBCD header) + CDHD size + VERB size + OBNA size
	uint32 totalSize = 8 + slot.cdhdBlock.size() + mergedVerb.size() + donor.obnaBlock.size();

	Common::Array<byte> result(totalSize);
	byte *out = result.data();

	// OBCD header
	writeBE32(out, MKTAG('O','B','C','D'));
	writeBE32(out + 4, totalSize);

	// CDHD (from slot — keeps identity/position)
	memcpy(out + 8, slot.cdhdBlock.data(), slot.cdhdBlock.size());

	// VERB (merged)
	memcpy(out + 8 + slot.cdhdBlock.size(), mergedVerb.data(), mergedVerb.size());

	// OBNA (from donor — carries the item name)
	memcpy(out + 8 + slot.cdhdBlock.size() + mergedVerb.size(),
	       donor.obnaBlock.data(), donor.obnaBlock.size());

	return result;
}

// ============================================================================
// Main implementation
// ============================================================================

Common::Error ScummEngine_v5::randomizeGameFiles() {
	debug(0, "Randomizer: Starting OBCD swap randomization...");

	// ===== Step 1: Catalog all pick-uppable objects =====
	Common::Array<ObcdCatalogEntry> catalog;

	// Map from (roomNo, objId) → index in catalog
	// Also track which rooms are modified
	Common::HashMap<int, bool> modifiedRooms;

	int numRooms = _res->_types[rtRoom].size();
	debug(0, "Randomizer: Scanning %d rooms...", numRooms);

	for (int roomIdx = 1; roomIdx < numRooms; roomIdx++) {
		// Skip rooms that don't have valid resource data
		if (_res->_types[rtRoom][roomIdx]._roomoffs == RES_INVALID_OFFSET)
			continue;

		// Skip rooms that aren't defined (no disk number assigned)
		if (_res->_types[rtRoom][roomIdx]._roomno == 0 && _res->_types[rtRoom][roomIdx]._roomoffs == 0)
			continue;

		// Load the room resource
		debug(0, "Randomizer: Loading room %d (disk %d, offset %u)...", roomIdx,
			_res->_types[rtRoom][roomIdx]._roomno, _res->_types[rtRoom][roomIdx]._roomoffs);
		byte *roomData = getResourceAddress(rtRoom, roomIdx);
		if (!roomData) {
			debug(0, "Randomizer: Room %d has no data, skipping", roomIdx);
			continue;
		}

		// Log what we got
		uint32 tag = blockTag(roomData);
		debug(0, "Randomizer: Room %d data tag: '%c%c%c%c', size: %u",
			roomIdx,
			(tag >> 24) & 0xFF, (tag >> 16) & 0xFF, (tag >> 8) & 0xFF, tag & 0xFF,
			blockSize(roomData));

		// Verify it's a ROOM block — if we got LFLF, find ROOM inside it
		if (tag == MKTAG('L','F','L','F')) {
			const byte *roomBlock = findSubBlock(roomData, MKTAG('R','O','O','M'));
			if (!roomBlock) {
				debug(0, "Randomizer: Room %d: LFLF has no ROOM child, skipping", roomIdx);
				continue;
			}
			roomData = const_cast<byte *>(roomBlock);
			tag = blockTag(roomData);
		}

		if (tag != MKTAG('R','O','O','M'))
			continue;

		// Iterate all OBCD blocks in this room
		ResourceIterator obcds(roomData, false);
		const byte *obcdPtr;
		int obcdCount = 0;
		while ((obcdPtr = obcds.findNext(MKTAG('O','B','C','D'))) != nullptr) {
			obcdCount++;
			uint32 obcdSize = blockSize(obcdPtr);

			// Find CDHD sub-block
			const byte *cdhdBlock = findSubBlock(obcdPtr, MKTAG('C','D','H','D'));
			if (!cdhdBlock)
				continue;

			// Read obj_id from CDHD data (skip 8-byte header)
			const byte *cdhdData = cdhdBlock + 8;
			uint16 objId = readLE16(cdhdData); // v5: obj_id is first field

			// Find VERB sub-block
			const byte *verbBlock = findSubBlock(obcdPtr, MKTAG('V','E','R','B'));
			if (!verbBlock)
				continue;

			uint32 verbBlockSize = blockSize(verbBlock);

			// Check if this object has 0x09 (pickup)
			Common::Array<VerbEntry> verbEntries = parseVerbEntries(verbBlock);
			
			// Debug: show obj id, name, and verbs
			const byte *obnaDbg = findSubBlock(obcdPtr, MKTAG('O','B','N','A'));
			const char *nameStr = obnaDbg ? (const char *)(obnaDbg + 8) : "?";
			Common::String verbList;
			for (const auto &ve : verbEntries) {
				if (!verbList.empty()) verbList += ",";
				verbList += Common::String::format("0x%x", ve.verbId);
			}
			debug(0, "Randomizer:   obj %d '%s' verbs=[%s]", objId, nameStr, verbList.c_str());
			
			bool hasPickUp = false;
			for (const auto &ve : verbEntries) {
				if (ve.verbId == 0x09) {
					hasPickUp = true;
					break;
				}
			}

			if (!hasPickUp)
				continue;

			// Find OBNA sub-block
			const byte *obnaBlock = findSubBlock(obcdPtr, MKTAG('O','B','N','A'));
			if (!obnaBlock)
				continue;

			// Build catalog entry
			ObcdCatalogEntry entry;
			entry.objId = objId;
			entry.roomNo = roomIdx;
			entry.cdhdBlock = copyBlock(cdhdBlock);
			entry.obnaBlock = copyBlock(obnaBlock);
			entry.verbBlock = copyBlock(verbBlock);
			entry.obcdBlock.resize(obcdSize);
			memcpy(entry.obcdBlock.data(), obcdPtr, obcdSize);
			// entry.verb14Script = extractVerb14Script(verbBlock, verbBlockSize);
			// entry.hasVerb14 = hasVerb14;

			const byte *obnaData = obnaBlock + 8;
			debug(1, "Randomizer: Room %d, obj %d '%s' — pick-uppable", roomIdx, objId, (const char *)obnaData);

			catalog.push_back(entry);
		}
	}

	debug(0, "Randomizer: Found %d pick-uppable objects", (int)catalog.size());

	if (catalog.size() < 2) {
		debug(0, "Randomizer: Not enough objects to randomize");
		return Common::kNoError;
	}

	// ===== Step 2: Build a shuffled mapping =====
	// Fisher-Yates shuffle: each slot i receives donor shuffled[i]
	Common::Array<int> indices(catalog.size());
	for (int i = 0; i < (int)catalog.size(); i++)
		indices[i] = i;

	Common::RandomSource rng("scummRandomizer");
	for (int i = (int)indices.size() - 1; i > 0; i--) {
		int j = rng.getRandomNumber(i);
		SWAP(indices[i], indices[j]);
	}

	// Log the mapping
	for (int i = 0; i < (int)catalog.size(); i++) {
		int j = indices[i];
		if (i != j) {
			const byte *slotName = catalog[i].obnaBlock.data() + 8;
			const byte *donorName = catalog[j].obnaBlock.data() + 8;
			debug(0, "Randomizer: Slot obj %d '%s' (room %d) ← Donor obj %d '%s' (room %d)",
				catalog[i].objId, (const char *)slotName, catalog[i].roomNo,
				catalog[j].objId, (const char *)donorName, catalog[j].roomNo);
			modifiedRooms[catalog[i].roomNo] = true;
		}
	}

	// ===== Step 4: Build merged OBCDs =====
	// Map from (roomNo, objId) → new OBCD bytes
	Common::HashMap<uint32, Common::Array<byte>> newObcds;

	for (int i = 0; i < (int)catalog.size(); i++) {
		int j = indices[i];
		if (i == j) continue; // no swap needed

		uint32 key = (catalog[i].roomNo << 16) | catalog[i].objId;
		newObcds[key] = buildMergedObcd(catalog[i], catalog[j]);
	}

	// ===== Step 5: Rebuild each modified room and write .001 =====
	// We need to rewrite the entire .001 file with updated room data.

	// First, read the original .001 file fully
	openRoom(1); // opens the resource file
	Common::Path origFilename = generateFilename(1);
	debug(0, "Randomizer: Resource file: %s", origFilename.toString().c_str());

	// Read the entire original file into memory
	Common::File origFile;
	if (!origFile.open(origFilename)) {
		warning("Randomizer: Cannot open original resource file '%s'", origFilename.toString().c_str());
		return Common::kReadingFailed;
	}
	uint32 origFileSize = origFile.size();
	Common::Array<byte> origData(origFileSize);
	origFile.read(origData.data(), origFileSize);
	origFile.close();

	// The .001 file has structure:
	// LECF (tag=4, size=4) [
	//   LOFF (tag=4, size=4) [ numRooms(1), {roomNo(1), offset(4)}×numRooms ]
	//   LFLF (tag=4, size=4) [ ... room data ... ] ×numRooms
	// ]
	//
	// Each LFLF contains the room's ROOM block (and possibly SOUN, COST, etc.)

	// Parse the LECF
	if (origFileSize < 8 || blockTag(origData.data()) != MKTAG('L','E','C','F')) {
		warning("Randomizer: Resource file doesn't start with LECF");
		return Common::kReadingFailed;
	}

	// Parse LOFF to find room offsets
	const byte *lecfData = origData.data();
	const byte *loffBlock = findSubBlock(lecfData, MKTAG('L','O','F','F'));
	if (!loffBlock) {
		warning("Randomizer: No LOFF block found");
		return Common::kReadingFailed;
	}
	int loffNumRooms = loffBlock[8]; // first byte after header

	struct LoffEntry {
		byte roomNo;
		uint32 offset; // relative to LECF start (i.e., file start)
	};
	Common::Array<LoffEntry> loffEntries;
	for (int i = 0; i < loffNumRooms; i++) {
		LoffEntry le;
		le.roomNo = loffBlock[9 + i * 5];
		le.offset = readLE32(loffBlock + 9 + i * 5 + 1);
		loffEntries.push_back(le);
	}

	// For each LFLF, rebuild it if the room is modified
	// We'll build the new file by: LECF header, LOFF (placeholder), then LFLFs

	Common::Array<byte> outputData;
	// Reserve space for LECF header (8 bytes)
	outputData.resize(8);
	writeBE32(outputData.data(), MKTAG('L','E','C','F'));
	// Size placeholder - will fill in later

	// Write LOFF block placeholder
	uint32 loffStartOffset = outputData.size();
	uint32 newLoffSize = 8 + 1 + loffNumRooms * 5; // header + numRooms byte + entries
	outputData.resize(outputData.size() + newLoffSize);
	byte *loffOut = outputData.data() + loffStartOffset;
	writeBE32(loffOut, MKTAG('L','O','F','F'));
	writeBE32(loffOut + 4, newLoffSize);
	loffOut[8] = loffNumRooms;
	// Entry offsets will be filled in as we write LFLFs

	// Sort LOFF entries by original offset to process them in file order
	Common::Array<int> loffOrder(loffNumRooms);
	for (int i = 0; i < loffNumRooms; i++)
		loffOrder[i] = i;
	for (int i = 0; i < loffNumRooms - 1; i++) {
		for (int j = i + 1; j < loffNumRooms; j++) {
			if (loffEntries[loffOrder[i]].offset > loffEntries[loffOrder[j]].offset)
				SWAP(loffOrder[i], loffOrder[j]);
		}
	}

	// Process each LFLF
	for (int ord = 0; ord < loffNumRooms; ord++) {
		int idx = loffOrder[ord];
		byte roomNo = loffEntries[idx].roomNo;
		uint32 lflfOrigOffset = loffEntries[idx].offset;

		// Record the new offset in the LOFF table
		uint32 newLflfOffset = outputData.size();
		byte *loffEntryOut = outputData.data() + loffStartOffset + 9 + idx * 5;
		loffEntryOut[0] = roomNo;
		writeLE32(loffEntryOut + 1, newLflfOffset);

		// Read the original LFLF block
		if (lflfOrigOffset + 8 > origFileSize) {
			warning("Randomizer: LFLF offset out of bounds for room %d", roomNo);
			return Common::kReadingFailed;
		}

		const byte *lflfOrig = origData.data() + lflfOrigOffset;
		if (blockTag(lflfOrig) != MKTAG('L','F','L','F')) {
			warning("Randomizer: Expected LFLF at offset %u for room %d", lflfOrigOffset, roomNo);
			return Common::kReadingFailed;
		}
		uint32 lflfOrigSize = blockSize(lflfOrig);

		if (!modifiedRooms.contains(roomNo)) {
			// Room not modified — copy verbatim
			uint32 writeStart = outputData.size();
			outputData.resize(writeStart + lflfOrigSize);
			memcpy(outputData.data() + writeStart, lflfOrig, lflfOrigSize);
			continue;
		}

		// Room IS modified — rebuild the LFLF
		// Walk the LFLF's children. For each child:
		//   - If it's a ROOM block, walk its children and substitute OBCDs
		//   - Otherwise, copy verbatim

		// Build new LFLF content
		Common::Array<byte> newLflfContent;
		// LFLF header placeholder
		newLflfContent.resize(8);
		writeBE32(newLflfContent.data(), MKTAG('L','F','L','F'));

		const byte *lflfChild = lflfOrig + 8;
		const byte *lflfEnd = lflfOrig + lflfOrigSize;

		while (lflfChild + 8 <= lflfEnd) {
			uint32 childTag = blockTag(lflfChild);
			uint32 childSize = blockSize(lflfChild);

			if (childSize < 8 || lflfChild + childSize > lflfEnd)
				break;

			if (childTag == MKTAG('R','O','O','M')) {
				// Rebuild the ROOM block with substituted OBCDs
				Common::Array<byte> newRoom;
				newRoom.resize(8);
				writeBE32(newRoom.data(), MKTAG('R','O','O','M'));

				const byte *roomChild = lflfChild + 8;
				const byte *roomEnd = lflfChild + childSize;

				while (roomChild + 8 <= roomEnd) {
					uint32 rcTag = blockTag(roomChild);
					uint32 rcSize = blockSize(roomChild);
					if (rcSize < 8 || roomChild + rcSize > roomEnd)
						break;

					if (rcTag == MKTAG('O','B','C','D')) {
						// Check if this OBCD should be replaced
						const byte *cdhd = findSubBlock(roomChild, MKTAG('C','D','H','D'));
						if (cdhd) {
							uint16 objId = readLE16(cdhd + 8);
							uint32 key = (roomNo << 16) | objId;
							if (newObcds.contains(key)) {
								// Substitute
								const Common::Array<byte> &replacement = newObcds[key];
								uint32 pos = newRoom.size();
								newRoom.resize(pos + replacement.size());
								memcpy(newRoom.data() + pos, replacement.data(), replacement.size());
								debug(1, "Randomizer: Replaced OBCD for obj %d in room %d (%u → %u bytes)",
									objId, roomNo, rcSize, (uint32)replacement.size());
								roomChild += rcSize;
								continue;
							}
						}
					}

					// Copy child verbatim
					uint32 pos = newRoom.size();
					newRoom.resize(pos + rcSize);
					memcpy(newRoom.data() + pos, roomChild, rcSize);
					roomChild += rcSize;
				}

				// Update ROOM size
				writeBE32(newRoom.data() + 4, newRoom.size());

				// Append to LFLF
				uint32 pos = newLflfContent.size();
				newLflfContent.resize(pos + newRoom.size());
				memcpy(newLflfContent.data() + pos, newRoom.data(), newRoom.size());
			} else {
				// Copy non-ROOM child verbatim
				uint32 pos = newLflfContent.size();
				newLflfContent.resize(pos + childSize);
				memcpy(newLflfContent.data() + pos, lflfChild, childSize);
			}

			lflfChild += childSize;
		}

		// Update LFLF size
		writeBE32(newLflfContent.data() + 4, newLflfContent.size());

		// Append to output
		uint32 writeStart = outputData.size();
		outputData.resize(writeStart + newLflfContent.size());
		memcpy(outputData.data() + writeStart, newLflfContent.data(), newLflfContent.size());
	}

	// Update LECF size
	writeBE32(outputData.data() + 4, outputData.size());

	// Write the output .001 file
	Common::DumpFile out001;
	Common::String out001Path = "randomizer/output/MONKEY2/MONKEY2.001";
	if (!out001.open(Common::Path(out001Path))) {
		warning("Randomizer: Cannot open output file '%s'", out001Path.c_str());
		return Common::kWritingFailed;
	}
	out001.write(outputData.data(), outputData.size());
	out001.close();
	debug(0, "Randomizer: Wrote %s (%u bytes)", out001Path.c_str(), (uint32)outputData.size());

	// ===== Step 6: Write updated .000 index =====
	// We need to update the DROO block in the .000 file with new room offsets.

	// Read the original .000
	(void)generateFilename(0);
	// For MI2, the index file is typically MONKEY2.000 — but generateFilename(0) 
	// may return the actual name. Let's try opening room 0.
	// Actually, the .000 is opened at the beginning via openRoom(0) in readIndexFile.
	// For v5 games, the index filename follows the same pattern.

	// The .000 file is the index file. Let's get its name differently.
	// For MI2 v5, the files are typically MONKEY2.000 and MONKEY2.001.
	// The generate function for room 0 should give us the index file name.
	Common::Path origIdx = generateFilename(0);
	debug(0, "Randomizer: Index file: %s", origIdx.toString().c_str());

	Common::File idxFile;
	if (!idxFile.open(origIdx)) {
		warning("Randomizer: Cannot open index file '%s'", origIdx.toString().c_str());
		return Common::kReadingFailed;
	}
	uint32 idxFileSize = idxFile.size();
	Common::Array<byte> idxData(idxFileSize);
	idxFile.read(idxData.data(), idxFileSize);
	idxFile.close();

	// Find the DROO block in the index and update room offsets
	// The .000 file is a series of blocks: [tag(4)][size(4)][data...]
	// DROO contains: numRooms(2 LE), then roomno[numRooms] (1 byte each),
	// then roomoffs[numRooms] (4 bytes LE each)
	{
		uint32 pos = 0;
		bool foundDROO = false;
		while (pos + 8 <= idxFileSize) {
			uint32 tag = readBE32(idxData.data() + pos);
			uint32 sz = readBE32(idxData.data() + pos + 4);
			if (sz < 8 || pos + sz > idxFileSize)
				break;

			if (tag == MKTAG('D','R','O','O')) {
				// Found DROO
				// Format: numRooms(2 LE), roomno[numRooms](1 each), roomoffs[numRooms](4 LE each)
				byte *drooData = idxData.data() + pos + 8;
				uint16 drooNumRooms = readLE16(drooData);
				byte *roomNos = drooData + 2;
				byte *roomOffs = roomNos + drooNumRooms;

				// Build a mapping from LOFF: roomNo → new file offset in the .001
				// The LOFF offsets are relative to the start of the .001 file.
				// The DROO roomoffs are also file offsets into the .001.
				// But wait — DROO stores the offset to the LFLF within the .001 file,
				// while LOFF stores the same. Actually, readRoomsOffsets reads from the
				// file after seeking to position 16, reading the LOFF block directly.
				// The offsets in readRoomsOffsets are offsets from the start of the file.
				// But actually looking more carefully: readRoomsOffsets reads from the 
				// resource file (the .001), seeking to offset 16, which is past the LECF+LOFF headers.
				// For v5 non-small-header: seek(16, SEEK_SET), then read byte count, 
				// then { roomNo(1), offset(4) } pairs.
				// The offsets stored there are absolute file offsets within the .001.
				// And in the DROO block, the offsets are stored but they get OVERWRITTEN
				// by readRoomsOffsets from the LOFF block in the .001.
				// So we don't actually need to update the DROO — the engine reads offsets
				// from LOFF at runtime! But let's update DROO anyway for consistency.

				for (uint16 r = 0; r < drooNumRooms; r++) {
					// Find this room in our new LOFF table
					for (int li = 0; li < loffNumRooms; li++) {
						uint32 newOff = readLE32(outputData.data() + loffStartOffset + 9 + li * 5 + 1);
						byte loffRoomNo = outputData.data()[loffStartOffset + 9 + li * 5];
						if (loffRoomNo == roomNos[r]) {
							writeLE32(roomOffs + r * 4, newOff);
							break;
						}
					}
				}

				foundDROO = true;
				break;
			}
			pos += sz;
		}

		if (!foundDROO) {
			warning("Randomizer: DROO block not found in index file");
		}
	}

	// Write the updated .000 file
	Common::DumpFile out000;
	Common::String out000Path = "randomizer/output/MONKEY2/MONKEY2.000";
	if (!out000.open(Common::Path(out000Path))) {
		warning("Randomizer: Cannot open output file '%s'", out000Path.c_str());
		return Common::kWritingFailed;
	}
	out000.write(idxData.data(), idxData.size());
	out000.close();
	debug(0, "Randomizer: Wrote %s (%u bytes)", out000Path.c_str(), (uint32)idxData.size());

	debug(0, "Randomizer: Done! %d objects randomized across %d modified rooms.",
		(int)catalog.size(), (int)modifiedRooms.size());

	return Common::kNoError;
}
