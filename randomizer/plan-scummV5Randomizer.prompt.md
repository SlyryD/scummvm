# Plan: Randomize SCUMM V5 Objects via OBCD Swapping

Swap OBCD blocks between pick-uppable objects so inventory items are randomized. Pick-uppable objects are defined as those that have the pickupObject opcode (depends on the engine, e.g., 0x25 in scumm v5) called on them in any script (complicating things, some scripts take the object id as an argument and call pickupObject on that). Each slot keeps its own CDHD (identity, position) and any scripts containing the pickupObject opcode (e.g., the pickup verb 0x09 in monkey2 frequently contains it), but receives the donor's OBNA (name) and other verb scripts. Donor verb bytecode is patched via an opcode-aware walker to replace the donor's `obj_id` with the slot's. Room blocks are rebuilt and LOFF/DROO offsets updated.

## Steps

### 1. Catalog all pick-uppable objects

In `randomizeGameFiles()` in `engines/scumm/randomizer.cpp`, iterate all global scripts (SCRP), local scripts (LSCR) inside rooms (ROOM), and object scripts (VERB) inside objects (OBCD). Rooms can be iterated via `_res->_types[rtRoom]` and `openRoom`. `ResourceIterator` can be used to find each OBCD.

On the first pass, iterate all of the scripts (LSCR, SCRP, VERB), keep track of:
- which scripts call `pickupObject` with a direct object id
- which scripts call `pickupObject` with an argument
- which scripts call each other (for transitive closure of indirect pickups)

On the second pass, keep track of which scripts call `pickupObject` with an argument that can be traced back to a direct object id. This gives us the full set of pick-uppable objects.

### 2. Build a shuffled mapping

Fisher-Yates shuffle the catalog indices using `Common::RandomSource`. Each slot *i* receives donor *j*'s non-pickup content.

### 3. Assemble merged OBCDs

For each swapped pair (slot *i*, donor *j*): build a new OBCD containing (a) slot *i*'s original CDHD (preserving `obj_id`, position, dimensions, flags), (b) a merged VERB block with the donor's verb entries and patched bytecode **except** for scripts with the pickupObject opcode which uses slot *i*'s original bytecode, and (c) the donor's OBNA. Recompute all block size headers (OBCD, VERB). Unswapped objects are kept verbatim.

### 4. Rebuild each modified room and write `.001`

For each room with swapped objects, walk the original ROOM block's children sequentially: copy RMHD, RMIM, OBIM, EXCD, ENCD, NLSC, LSCR verbatim; substitute each OBCD matched by `obj_id`. Update ROOM and LFLF size headers. Write the full `.001` to `randomizer/output/MONKEY2/MONKEY2.001` via `Common::DumpFile`: LECF header → LOFF block (with placeholder offsets) → all LFLF blocks → seek back to patch LOFF entries and LECF size.

### 5. Write updated `.000` index

Copy the original `.000` to output. Locate the DROO block and overwrite its `roomoffs[]` array (each entry is 4 bytes LE) with the new room offsets from the LOFF table.

### Further considerations

Putting lots of debug log statements throughout is encouraged.
