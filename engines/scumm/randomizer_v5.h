#ifndef SCUMM_RANDOMIZER_V5_H
#define SCUMM_RANDOMIZER_V5_H

#include "scumm/scumm_v5.h"
#include "common/array.h"
#include "common/str.h"

namespace Scumm {

// Struct for tracking all objects and their pickup status
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

class Randomizer_v5 : public ScummEngine_v5 {
public:
	struct PickupCall {
		uint16 objectId;
		uint16 room;
		uint32 scriptNum;
		uint16 scriptOffset;
	};

	Randomizer_v5(OSystem *syst, const DetectorResult &dr);
	Common::Error run() override;

protected:
	void setupScumm(const Common::Path &macResourceFile) override;
	void setupOpcodes() override;
	void o5_pickupObject();

private:
	struct DiscoveryAction {
		enum ActionType {
			kWait,
			kRoomExit,
			kSentence,
			kObjectScript,
			kGlobalScript,
			kUserInput,
			kUnknown
		};

		ActionType type;
		uint16 roomId;
		uint16 objectA;
		uint16 objectB;
		uint16 egoObject;
		byte verbId;
		uint32 scriptNum;
		uint16 scriptOffset;
		int waitTicks;
		Common::String label;
	};

	struct DiscoveryStateSnapshot {
		uint32 hash;
		int roomId;
		int roomResource;
	};

	struct DiscoveryTraceStep {
		uint32 parentHash;
		uint32 stateHash;
		int roomId;
		uint16 objectA;
		uint16 objectB;
		uint16 egoObject;
		byte verbId;
		uint32 scriptNum;
		uint16 scriptOffset;
		DiscoveryAction::ActionType actionType;
		Common::String actionLabel;
	};

	enum Room108InputStage {
		kRoom108PressEnter,
		kRoom108RecipeOnes,
		kRoom108SelectFullGame,
		kRoom108Complete
	};

	Common::Array<PickupCall> _pickupCalls;
	Common::Array<DiscoveryTraceStep> _discoveryTrace;
	Common::Array<DiscoveryTraceStep> _discoveryInputAttempts;
	Common::HashMap<uint32, bool> _discoverySeenStates;
	Common::HashMap<uint16, bool> _discoverySeenRooms;
	Common::Error randomize();
	
	// Discovery safeguards
	static const int MAX_DISCOVERY_RECURSION = 10;
	static const int MAX_DISCOVERY_DRAIN_CYCLES = 20;
	static const int MAX_DISCOVERY_WAIT_STEPS = 8;
	static const int DISCOVERY_WAIT_QUANTUM = 30;
	static const int DISCOVERY_CHECKPOINT_FIRST_SLOT = 50;
	static const int DISCOVERY_CHECKPOINT_SLOTS = 50;
	static const uint32 MAX_DISCOVERY_STATES = 10000;
	int _discoveryRecursionDepth;
	uint32 _discoveryDrainCount;
	int _nextDiscoveryCheckpointSlot;
	static const uint32 MAX_DISCOVERY_INSTRUCTIONS = 100000;
	bool _discoveryMode = false;
	Room108InputStage _room108InputStage = kRoom108PressEnter;
	int _room108RecipeInputs = 0;

	void beginDiscoveryGraphSearch();
	uint32 hashDiscoveryState(const DiscoveryStateSnapshot &state) const;
	void snapshotDiscoveryState(DiscoveryStateSnapshot &out);
	void checkpointDiscoveryState();
	void writeDiscoveryReport(const Common::Array<ObjCatalogEntry> &catalog, const Common::Array<int> &pickuppableIndices) const;
	void recordDiscoveryTrace(const DiscoveryTraceStep &step);
	bool registerDiscoveryState(uint32 hash);
	void exploreReachableStateGraph(Common::Array<ObjCatalogEntry> &catalog, Common::Array<int> &outPickuppableIndices);
	void initializeDiscoveryScriptVariables();
	void resetDiscoveryState();
	void prepareRoomForDiscovery(int roomId);
	void setupDiscoveryRoomObjects();
	void drainDiscoveryScripts();
	void runDiscoverySentence(int roomId, uint16 objectA, uint16 objectB, byte verbId);
	void runDiscoveryObjectScript(int roomId, uint16 objId, byte verbId);
	bool runRoom108UserInput(DiscoveryTraceStep &outTrace);
	void discoverReachableRoom(Common::Array<ObjCatalogEntry> &catalog, Common::HashMap<uint16, int> &objToCatalogIdx, int roomId);
	void executeScriptAtOffset(uint16 scriptNumber, uint32 scriptOffset, byte where, int *vars = nullptr);
};

} // End of namespace Scumm

#endif