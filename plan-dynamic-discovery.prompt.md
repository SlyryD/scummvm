## Plan: Brute-Force the State Graph and Record Everything

TL;DR: search the logical SCUMM game state graph, not the raw player inputs. The planner runs the game's boot script to establish the actual initial state, snapshots it, enumerates valid SCUMM transitions, and follows only rooms reached through native game behavior. It then explores the reachable graph with BFS/A* while logging every transition, script path, and pickup event. This models gameplay as state changes, not as fake “walk this pixel path” heuristics.

**Canonical state**
1. Room and room-resource identity, current actor locations, and room-local object ownership/state.
2. Inventory and object ownership, with the precise pickup target and actor/object relationship, for objects encountered in dynamically reachable states.
3. Variables, locals, bit variables, flags, and room variables that affect script control flow or object availability.
4. A canonical hash of meaningful game state plus a private in-memory save blob that can restore actor positions and movement state, object locations and states, inventory ownership, room-local state, variables, flags, timers, script slots, locals, cutscene state, and future scheduled work for branch isolation.

Sentence context is transition metadata, not part of the stable node identity. The search does not need to model the brief interval between `doSentence()` adding a `SentenceTab` and the sentence script consuming it. Each sentence action records its verb, primary object, secondary object, and ego actor on the edge, then runs through the native scheduler to a stable post-action snapshot. This prevents transient pending-sentence states from creating artificial graph nodes.

The search uses stable post-action states. Every action is executed through the native interpreter and scheduler until a defined boundary: no immediately runnable scripts, no pending sentence, and no discovery-specific instruction or drain limit reached. Delayed scripts, timers, actor movement, and other scheduled future work are preserved in the serialized engine snapshot whenever they can affect a later transition. Script number and bytecode offset are recorded as execution provenance on the transition trace and pickup diagnostics, but are not part of the canonical node identity. If a branch cannot reach the boundary within its limits, it is recorded as bounded or incomplete rather than treated as an ordinary stable state.

Branch isolation uses the existing SCUMM serializer through private helpers in `Randomizer_v5`; it does not modify the shared save-state files. Each node stores its canonical hash and a private in-memory blob containing the serialized engine state. The blob is an implementation detail used to resume execution; its transient VM fields do not automatically become part of the graph identity. The helper must reproduce the load preparation that the normal save loader performs, including clearing dynamic resources and restoring room/resource state, before deserializing a branch.

**Steps**
1. Initialize the dynamic search from the real beginning of the game by resetting the discovery engine, calling `runBootscript()`, and draining its immediately runnable work to the first stable post-action state. Do not seed the dynamic search by iterating through room IDs or directly loading every room.
2. Serialize that booted state into a private in-memory blob through `saveLoadWithSerializer()` and create the canonical node hash from meaningful game state. Use the blob for all future branch restoration.
3. Define the canonical state and serializer boundary explicitly: the blob must preserve actors, objects, inventory, variables, flags, script slots, locals, delays, cutscene state, and future scheduled work; the graph hash should include meaningful gameplay state rather than transient VM cursor details.
4. Build a stable state hash from the canonical state so repeated states are pruned, cycles are detected, and the search graph is flattened to unique reachable SCUMM states.
5. Generate valid actions at the engine abstraction layer, not as raw keyboard commands. Enumerate objects, verbs, movement targets, room exits, and waits from the currently restored reachable state; do not prepopulate actions from every room resource:
	- sentence actions via `doSentence(verb, objectA, objectB)`
	- room transitions produced by native room-loading opcodes, exit scripts, and actor movement; `startScene()` is the engine implementation of such a transition, not a free action to any room ID
	- native script entrypoints via `runObjectScript(object, entry, ...)`
	- actor movement such as `walkActorToObject()` / `walkActorToActor()` for triggers and room transitions
	- explicit `wait`/delay actions for animation- and timer-based gates; each wait advances engine time by a bounded quantum or to the next relevant timer event, then drains newly runnable work
6. Explore each outgoing edge from an isolated parent: restore the parent's in-memory blob using the private discovery load helper, execute exactly one meaningful action through the native interpreter and scheduler, drain all immediately runnable work, and serialize the resulting child into a new blob. Never execute sibling edges from a mutated parent engine state.
7. Add bounded search controls: max states, max actions per state, max script time, max branching factor, and max instruction budget per probe; if a branch hits a timeout or recursion boundary, log it and continue.
8. Record only meaningful graph edges: parent hash, child hash, action type, room before/after, verb, primary object, secondary object, ego object, scripts and offsets entered, state delta, completion status, and pickup payload when captured. Sentence context and script cursor data belong here as edge metadata and execution provenance, not in the state hash.
9. Record all discovered pickup targets with ancestry: the exact `o5_pickupObject` parameters, the triggering state, and the action sequence that reached it. Objects that cannot be reached through the native graph are intentionally not classified as pickuppable by this search.
10. After the reachable frontier is exhausted, export a summary of unique states, reachable rooms, dead ends, repeated states, and successful progression chains so they can be reviewed without replaying every raw command.

**Real SCUMM action mapping**
- `runBootscript()` is the dynamic search entry point. It runs the game's boot script, normally script 1, which establishes the initial variables, inventory, actors, and starting room through native SCUMM behavior.
- The dynamic search must not call `startScene()` with arbitrary room IDs to manufacture adjacency. A room becomes reachable only when the boot script or a later valid action causes the native interpreter to load it through an exit, movement trigger, sentence script, or other game-authored transition.
- “enter trigger” is not a separate invented action. In SCUMM it is represented by actor movement plus the script/trigger state that fires when the actor reaches the hotspot, room boundary, or object interaction region.
- “leave scene” is the normal room transition path implemented by `startScene()` and the room exit script flow in [engines/scumm/room.cpp](engines/scumm/room.cpp).
- “wait until cutscene resolves” is implemented by the native script scheduler and timing system, not by a special fake action. It corresponds to the script loop, `breakHere`, delays, and pending script execution in [engines/scumm/script.cpp](engines/scumm/script.cpp).
- The planner should never invent names like `enter_trigger` or `leave_scene`; it should use the engine's real semantics and log them as room transitions, script triggers, movement, and waits.
- Static inspection of every room resource is not part of discovery. It can report objects that exist in files, but it cannot establish that they are playable or pickuppable, so it is excluded from the graph and pickup classification.

**Relevant files**
- [engines/scumm/randomizer_v5.h](engines/scumm/randomizer_v5.h) — planner state, action queue, trace buffer, and discovery metadata.
- [engines/scumm/randomizer_v5.cpp](engines/scumm/randomizer_v5.cpp) — state snapshotting, dynamic action generation, graph search, pickup capture, and trace export.
- [engines/scumm/script.cpp](engines/scumm/script.cpp) — native scheduler, sentence flow, and script execution used to drive valid actions.
- [engines/scumm/room.cpp](engines/scumm/room.cpp) — room transitions and scene setup.
- [engines/scumm/object.cpp](engines/scumm/object.cpp) — object ownership, OBCD/verb lookups, and room object state.
- [engines/scumm/scumm.cpp](engines/scumm/scumm.cpp) — `runBootscript()` and game initialization.
- [engines/scumm/scumm_v5.h](engines/scumm/scumm_v5.h) — v5-local seam for discovery-only tracing or suppression.
- [engines/scumm/script_v5.cpp](engines/scumm/script_v5.cpp) — opcode/operand behavior for validation and guard assumptions.

**Verification**
1. Confirm the planner can serialize the booted state, restore it repeatedly with the local load preparation, and execute sibling actions without cross-branch mutation.
2. Validate that the action generator uses valid SCUMM transitions only and does not rely on raw button sequences.
3. Run a small bounded search and verify repeated states are pruned correctly and cycles are logged.
4. Confirm `o5_pickupObject` events are recorded with the full ancestry of actions that reached the pickup state.
5. Verify traces include room, object, script, verb, state hash, and action metadata for every branch.
6. Smoke-test the standard randomizer flow to ensure the search code remains discovery-only and does not affect normal gameplay paths.

**Decisions**
- Search the logical game state graph, not the player’s physical movement path.
- Use dynamic reachable-state discovery as the only source of candidate objects, verbs, room transitions, and pickup results; do not use static all-room cataloging.
- Start dynamic exploration from `runBootscript()` and follow only room transitions produced by valid native behavior; do not iterate over all room IDs as graph roots.
- Use real SCUMM actions such as `doSentence()`, native room-loading behavior, `runObjectScript()`, and actor movement instead of hand-authored pseudo-events or arbitrary `startScene()` calls.
- Use `saveLoadWithSerializer()` through private in-memory helpers in `Randomizer_v5`; do not modify `scumm.h` or `saveload.cpp`, and do not duplicate the serializer field list.
- Use stable post-action states: drain immediately runnable work before hashing, preserve future timer/scheduling state in the serialized snapshot, and keep current script slot, script pointer, and pending sentence data out of the canonical state hash.
- Treat sentence context `(verb, objectA, objectB, egoObject)` and script number/offset provenance as edge metadata; record incomplete branches separately when they do not reach the stable boundary.
- Treat `wait` as an explicit action for timer-based and animation-based gates.
- Record all branches and prune only by canonical state equality, not by guesswork about player intent.
- Keep all graph logging and trace export inside the discovery path so the main runtime behavior remains unchanged.

**Further Considerations**
1. The search is intentionally limited to the dynamically reachable graph. Objects or rooms that require an unmodeled action remain unknown rather than being inferred from static resource presence.
2. The private load helper must mirror the normal loader's preparation closely enough for repeated restoration: close the current room, clear discovery-owned transient data, nuke/reload dynamic resources as required, deserialize the blob, and run the local post-load fixups needed by v5.
3. Delete a child blob immediately when its canonical hash is already known, and release a fully expanded node's blob when no queued edge needs it. Keep blobs for queued frontier nodes.
4. Add a restoration determinism check: save state A, mutate through an action, restore A, execute the same action again, and verify the resulting canonical hashes match.
5. If the reachable state space gets too large, switch from pure BFS to A* or best-first search with a heuristic based on inventory progress and room reachability.
6. For time-dependent puzzles, prefer engine-time advances rather than fake “human reaction” simulations; a wait edge should advance to a bounded quantum or the next relevant timer event and retain any future scheduled work needed to reproduce the state.
7. If some actions are unreachable via the sentence path, add a narrowly scoped fallback through the native script entrypoint path, but keep that fallback explicit and logged.
