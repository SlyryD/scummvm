# Plan: Randomize SCUMM V5 Objects via OBCD Swapping

Swap OBCD blocks between pick-uppable objects so inventory items are randomized. Each slot keeps its own CDHD (identity, position) and verb-14 pickup script, but receives the donor's OBNA (name) and non-pickup verb scripts. Donor verb bytecode is patched via an opcode-aware walker to replace the donor's `obj_id` with the slot's. Room blocks are rebuilt and LOFF/DROO offsets updated.

## Steps

### 1. Catalog all pick-uppable objects

In `randomizeGameFiles()` in `engines/scumm/randomizer.cpp`, iterate every room via `_res->_types[rtRoom]` and `openRoom`. For each room, read the ROOM block and use `ResourceIterator` to find each OBCD. For each OBCD, extract the `obj_id` from CDHD, the full CDHD bytes, the OBNA bytes, and parse the VERB block: scan its entry table for verb `0x0E` (pickup). Record pick-uppable objects in a catalog struct: `{obj_id, room_no, cdhd_bytes, obna_bytes, verb14_script_bytes, full_verb_block_bytes}`.

### 2. Build a shuffled mapping

Fisher-Yates shuffle the catalog indices using `Common::RandomSource`. Each slot *i* receives donor *j*'s non-pickup content.

### 3. Build an opcode-aware bytecode patcher

Implement a function that walks SCUMM V5 bytecode instruction-by-instruction using a parameter-signature table derived from the `setupOpcodes` mapping in `engines/scumm/script_v5.cpp`. For each instruction, use the PARAM bits in the opcode byte to determine parameter sizes (direct literal vs. variable ref). When a **direct word** at a known object-ID parameter position (e.g., PARAM_1 of `setState`/`setOwnerOf`/`pickupObject`/`drawObject`, PARAM_2 of `faceActor`/`walkActorToObject`/`putActorInRoom`) matches the donor's `obj_id`, replace it with the slot's `obj_id`. Skip inline strings (scan to NUL), sub-opcode loops (walk to `0xFF` terminator), and varargs.

### 4. Assemble merged OBCDs

For each swapped pair (slot *i*, donor *j*): build a new OBCD containing (a) slot *i*'s original CDHD (preserving `obj_id`, position, dimensions, flags), (b) a merged VERB block with the donor's verb entries and patched bytecode **except** verb-14 which uses slot *i*'s original bytecode, and (c) the donor's OBNA. Recompute all block size headers (OBCD, VERB). Unswapped objects are kept verbatim.

### 5. Rebuild each modified room and write `.001`

For each room with swapped objects, walk the original ROOM block's children sequentially: copy RMHD, RMIM, OBIM, EXCD, ENCD, NLSC, LSCR verbatim; substitute each OBCD matched by `obj_id`. Update ROOM and LFLF size headers. Write the full `.001` to `randomizer/output/MONKEY2/MONKEY2.001` via `Common::DumpFile`: LECF header → LOFF block (with placeholder offsets) → all LFLF blocks → seek back to patch LOFF entries and LECF size.

### 6. Write updated `.000` index

Copy the original `.000` to output. Locate the DROO block and overwrite its `roomoffs[]` array (each entry is 4 bytes LE) with the new room offsets from the LOFF table.

## Further Considerations

1. **Opcode walker completeness** — The walker needs ~65 handler signatures. The most complex cases are `print`/`printEgo` (inline text + sub-opcodes), `expression` (recursive opcode), and `actorOps`/`verbOps`/`roomOps` (sub-opcode loops). For a first pass, we could handle the common fixed-signature opcodes and treat unknown/complex opcodes as a warning (log and skip that verb script without patching). Want to go with this incremental approach, or implement the full walker upfront?
