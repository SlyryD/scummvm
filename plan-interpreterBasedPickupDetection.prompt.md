# Plan: Refactor to Use Interpreter-Based Pickup Detection

**TL;DR** Create `Randomizer_v5` as a subclass of `ScummEngine_v5` that overrides the script interpreter to track `o5_pickupObject` calls in real time. Under `RANDOMIZER`, select `Randomizer_v5` instead of `ScummEngine_v5`, and perform discovery plus file randomization from `Randomizer_v5::go()`. Execute all game scripts (SCRP, LSCR, VERB) with a dummy actor and room context to discover pickuppable objects via the actual bytecode interpreter instead of static analysis.

## Steps

### 1. Create `engines/scumm/randomizer_v5.h` and `engines/scumm/randomizer_v5.cpp`
Define `class Randomizer_v5 : public ScummEngine_v5` with:
- Constructor that takes the same args as `ScummEngine_v5` but disables graphics/sound setup
- Member to store collected pickup calls:
  ```cpp
  struct PickupCall {
    uint16 objectId;
    uint16 room;
    uint32 scriptNum;
    uint16 scriptOffset;
  };
  Common::Array<PickupCall> _pickupCalls;
  ```
- *depends on Step 2*

### 2. Override `o5_pickupObject()` to capture instead of execute
- Extract `obj = getVarOrDirectWord(PARAM_1); room = getVarOrDirectByte(PARAM_2);`
- Store `{obj, room, _currentScript, _scriptPointer}` in the tracking array
- Skip the side-effect calls (`addObjectToInventory`, `putOwner`, etc.)
- *parallel with Step 3*

### 3. Override `go()` and related game-loop methods to prevent rendering
Methods to stub/override:
- `go()` — replace the normal gameplay startup path with script discovery plus randomization output generation
- `scummLoop()`, `scummLoop_handleActors()`, `scummLoop_handleSaveLoad()` — empty stubs
- `drawFrame()`, `updateScreen()` — stubs
- Keep `runInventoryScript()` executing normally so nested inventory-script-driven `pickupObject` calls are still discovered
- Stub only rendering/UI/animation paths that are not required for script control flow and would otherwise try to present the game
- *parallel with Step 4*

### 4. Enumerate and execute all scripts
Create a method `discoverPickuppableObjects()` that:
- Sets up a minimal dummy context: actor 0 in room 0
- Iterates all global scripts (SCRP) via `_res->_types[rtScript]` and `ResourceIterator`
- Iterates all rooms (ROOM) via `_res->_types[rtRoom]` and for each room:
  - Executes local scripts (LSCR)
  - Iterates all objects (OBCD) in the room and executes their verb scripts (VERB)
- Calls `executeScript()` on each script number found
- Returns the tracking array of all captured `PickupCall` entries
- *depends on Step 2, Step 3*

### 5. Extract catalog from tracked calls
Process the `PickupCall` array:
- Group by `(objectId, room)` to get unique pick-uppable object instances
- Validate each pickup target (similar to current `validatePickupCandidate`, but using runtime context from the script that called it)
- Build the catalog of pickuppable objects
- *depends on Step 4*

### 6. Make `RANDOMIZER` select `Randomizer_v5`, and perform the pipeline in `Randomizer_v5::go()`
- Update the version-5 engine factory branch in `engines/scumm/metaengine.cpp` so it instantiates `Randomizer_v5` under `#ifdef RANDOMIZER`, and `ScummEngine_v5` otherwise
- Add the `Randomizer_v5` header include to `engines/scumm/metaengine.cpp`
- Simplify `ScummEngine::run()` in `engines/scumm/scumm.h` so it always does `init()` then dispatches to virtual `go()`, removing the current `#ifdef RANDOMIZER` branch that calls `randomizeGameFiles()`
- Implement `Randomizer_v5::go()` to:
  - Call `discoverPickuppableObjects()` to get the catalog
  - Proceed with the existing shuffle, merge, rebuild, and write pipeline
  - Return the final `Common::Error` directly from the randomizer engine flow
- Remove the need for a separate `randomizeGameFiles()` engine API
- *depends on Step 5*

## Relevant Files
- [engines/scumm/randomizer.cpp](engines/scumm/randomizer.cpp) — current randomizer pipeline logic to migrate under `Randomizer_v5::go()`
- [engines/scumm/script_v5.cpp](engines/scumm/script_v5.cpp) — reference implementation of `o5_pickupObject()` for interpreter-driven pickup capture
- [engines/scumm/metaengine.cpp](engines/scumm/metaengine.cpp) — SCUMM engine factory; version-5 branch should pick `Randomizer_v5` under `RANDOMIZER`
- [engines/scumm/scumm.h](engines/scumm/scumm.h) — `ScummEngine::run()` should always dispatch to virtual `go()` after `init()`
- [engines/scumm/scumm_v5.h](engines/scumm/scumm_v5.h) — base class with protected interpreter state such as `_opcode` and `_scriptPointer`
- `engines/scumm/randomizer_v5.h` — new `Randomizer_v5` class declaration
- `engines/scumm/randomizer_v5.cpp` — new `Randomizer_v5` implementation

## Decisions
- **All scripts** executed (not subset) to maximize pickup discovery, especially for indirect/variable object IDs
- **Separate files** (`randomizer_v5.h/cpp`) keep the interpreter subclass isolated and maintainable
- **Dummy actor/room** context (actor 0, room 0) minimizes initialization overhead while satisfying interpreter requirements
- **Track all calls** (not just unique) to preserve script origin for debugging and to handle repeated calls in different control paths

## Verification
1. Compile `Randomizer_v5` and verify no link errors
2. Create a test that instantiates `Randomizer_v5` with Monkey Island 2 data and calls `discoverPickuppableObjects()`
3. Compare discovered pickup calls against the current static extraction results (should be >= current count, ideally higher due to indirect/variable object IDs)
4. Run full randomization pipeline and verify `.001`/`.000` output generation succeeds
5. Spot-check object offsets in generated files against expected ranges

## Further Considerations

### Script execution side effects
Some scripts may try to modify game state (e.g., set variables, start other scripts). Should we allow them to proceed, or stub those methods too?
- **Recommendation**: allow normal script execution, including nested `runScript()` and `runInventoryScript()` calls, and stub only presentation-heavy or environment-dependent behavior such as rendering, screen updates, and non-essential animation/UI paths.

### Script entrypoints
Do we run scripts from their natural entry points (e.g., local script entry 0), or from offset 0?
- **Recommendation**: entrypoint 0 (typical entry), but verify with a script sample.

### Timeout/infinite-loop protection
Should we add a recursion depth or cycle limit to prevent runaway script execution?
- **Recommendation**: add as an enhancement if needed, but likely not necessary for initial version.
