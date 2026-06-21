#ifndef SCUMM_RANDOMIZER_V5_H
#define SCUMM_RANDOMIZER_V5_H

#include "scumm/scumm_v5.h"
#include "common/array.h"

namespace Scumm {

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
	void setupOpcodes() override;
	void o5_pickupObject();
	void o5_loadRoom();
	void o5_loadRoomWithEgo();
	void o5_putActor();
	void o5_putActorAtObject();
	void o5_putActorInRoom();
	void o5_walkActorTo();
	void o5_walkActorToActor();
	void o5_walkActorToObject();

private:
	Common::Array<PickupCall> _pickupCalls;
	Common::Error randomize();

	void resetDiscoveryState();
	void prepareRoomForDiscovery(int roomId);
	void executeScriptAtOffset(uint16 scriptNumber, uint32 scriptOffset, byte where, int *vars = nullptr);
};

} // End of namespace Scumm

#endif