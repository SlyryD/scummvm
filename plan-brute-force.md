## Plan: Brute-Force the State Graph and Record Everything

TL;DR: search the logical SCUMM game state graph, not the raw player inputs. The planner snapshots the real engine state, enumerates valid SCUMM transitions (sentence actions, room transitions, script entrypoints, movement, and wait events), then explores the graph with BFS/A* while logging every transition, script path, and pickup event. This models gameplay as state changes, not as fake “walk this pixel path” heuristics.

**Canonical state**
1. Room and room-resource identity, current actor locations, and room-local object ownership/state.
2. Inventory and object ownership, with the precise pickup target and actor/object relationship.
3. Variables, locals, bit variables, flags, and room variables that affect script control flow or object availability.
4. The object/actor state needed to restore and re-execute the same SCUMM transition without mutating the base discovery world.

Sentence context is transition metadata, not part of the stable node identity. The search does not need to model the brief interval between `doSentence()` adding a `SentenceTab` and the sentence script consuming it. Each sentence action records its verb, primary object, secondary object, and ego actor on the edge, then runs through the native scheduler to a stable post-action snapshot. This prevents transient pending-sentence states from creating artificial graph nodes.

The search uses stable post-action states. Every action is executed through the native interpreter and scheduler until a defined boundary: no immediately runnable scripts, no pending sentence, and no discovery-specific instruction or drain limit reached. Delayed scripts, timers, actor movement, and other scheduled future work are not discarded; their durable state and remaining timing information are included whenever they can affect a later transition. Script number and bytecode offset are recorded as execution provenance on the transition trace and pickup diagnostics, but are not part of the canonical node identity. If a branch cannot reach the boundary within its limits, it is recorded as bounded or incomplete rather than treated as an ordinary stable state.

**Steps**
1. Define a canonical game-state snapshot that includes room, actor placement, object owners/states, inventory, local/bit variables, room resources, and durable VM-visible state required to restore a stable branch. Do not include the transient current script slot, script pointer, or pending sentence queue.
2. Build a stable state hash from the snapshot so repeated states are pruned, cycles are detected, and the search graph is flattened to unique SCUMM states.
3. Generate valid actions at the engine abstraction layer, not as raw keyboard commands:
	- sentence actions via `doSentence(verb, objectA, objectB)`
	- room transitions via `startScene(room, actor, objectNr)`
	- native script entrypoints via `runObjectScript(object, entry, ...)`
	- actor movement such as `walkActorToObject()` / `walkActorToActor()` for triggers and room transitions
	- explicit `wait`/delay actions for animation- and timer-based gates; each wait advances engine time by a bounded quantum or to the next relevant timer event, then drains newly runnable work
4. Execute each action through the real interpreter and scheduler rather than simulating player movement or injecting fake room events. Drain all immediately runnable work before snapshotting the child, while preserving delayed scripts, timers, actor movement, and other scheduled future work that can affect later transitions. Movement, scene changes, and cutscene resolution are state transitions driven by the engine.
5. Add bounded search controls: max states, max actions per state, max script time, max branching factor, and max instruction budget per probe; if a branch hits a timeout or recursion boundary, log it and continue.
6. Record a complete trace for every explored transition: parent hash, child hash, action type, room, verb, primary object, secondary object, ego object, scripts and offsets entered, state delta, completion status, and pickup payload when captured. Sentence context and script cursor data belong here as edge metadata and execution provenance, not in the state hash.
7. Record all discovered pickup targets with ancestry: the exact `o5_pickupObject` parameters, the triggering state, and the action sequence that reached it.
8. After the frontier is exhausted, export a summary of unique states, dead ends, repeated states, and successful progression chains so they can be reviewed without replaying every raw command.

**Real SCUMM action mapping**
- “enter trigger” is not a separate invented action. In SCUMM it is represented by actor movement plus the script/trigger state that fires when the actor reaches the hotspot, room boundary, or object interaction region.
- “leave scene” is the normal room transition path implemented by `startScene()` and the room exit script flow in [engines/scumm/room.cpp](engines/scumm/room.cpp).
- “wait until cutscene resolves” is implemented by the native script scheduler and timing system, not by a special fake action. It corresponds to the script loop, `breakHere`, delays, and pending script execution in [engines/scumm/script.cpp](engines/scumm/script.cpp).
- The planner should never invent names like `enter_trigger` or `leave_scene`; it should use the engine’s real semantics and log them as room transitions, script triggers, movement, and waits.

**Relevant files**
- [engines/scumm/randomizer_v5.h](engines/scumm/randomizer_v5.h) — planner state, action queue, trace buffer, and discovery metadata.
- [engines/scumm/randomizer_v5.cpp](engines/scumm/randomizer_v5.cpp) — state snapshotting, action generation, graph search, and trace export.
- [engines/scumm/script.cpp](engines/scumm/script.cpp) — native scheduler, sentence flow, and script execution used to drive valid actions.
- [engines/scumm/room.cpp](engines/scumm/room.cpp) — room transitions and scene setup.
- [engines/scumm/object.cpp](engines/scumm/object.cpp) — object ownership, OBCD/verb lookups, and room object state.
- [engines/scumm/scumm_v5.h](engines/scumm/scumm_v5.h) — v5-local seam for discovery-only tracing or suppression.
- [engines/scumm/script_v5.cpp](engines/scumm/script_v5.cpp) — opcode/operand behavior for validation and guard assumptions.

**Verification**
1. Confirm the planner can execute each action to the stable post-action boundary, preserve delayed work and timer state, then snapshot and restore that state without mutating the base discovery world.
2. Validate that the action generator uses valid SCUMM transitions only and does not rely on raw button sequences.
3. Run a small bounded search and verify repeated states are pruned correctly and cycles are logged.
4. Confirm `o5_pickupObject` events are recorded with the full ancestry of actions that reached the pickup state.
5. Verify traces include room, object, script, verb, state hash, and action metadata for every branch.
6. Smoke-test the standard randomizer flow to ensure the search code remains discovery-only and does not affect normal gameplay paths.

**Decisions**
- Search the logical game state graph, not the player’s physical movement path.
- Use real SCUMM actions such as `doSentence()`, `startScene()`, `runObjectScript()`, and actor movement instead of hand-authored pseudo-events.
- Use stable post-action states: drain immediately runnable work before hashing, preserve future timer/scheduling state that affects behavior, and keep current script slot, script pointer, and pending sentence data out of the canonical state.
- Treat sentence context `(verb, objectA, objectB, egoObject)` and script number/offset provenance as edge metadata; record incomplete branches separately when they do not reach the stable boundary.
- Treat `wait` as an explicit action for timer-based and animation-based gates.
- Record all branches and prune only by canonical state equality, not by guesswork about player intent.
- Keep all graph logging and trace export inside the discovery path so the main runtime behavior remains unchanged.

**Further Considerations**
1. If the state space gets too large, switch from pure BFS to A* or best-first search with a heuristic based on inventory progress and room reachability.
2. For time-dependent puzzles, prefer engine-time advances rather than fake “human reaction” simulations; a wait edge should advance to a bounded quantum or the next relevant timer event and retain any future scheduled work needed to reproduce the state.
3. If some actions are unreachable via the sentence path, add a narrowly scoped fallback through the native script entrypoint path, but keep that fallback explicit and logged.
