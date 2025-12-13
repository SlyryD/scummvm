# SCUMM Game Randomizer

This tool reads SCUMM index (.000) and data (.001) files, randomizes game objects, and writes modified files that can be used with ScummVM.

## Features

- **Object Ownership Randomization**: Changes which rooms or characters own objects
- **Object State Randomization**: Modifies object states (open/closed, visible/hidden, etc.)
- **Object Class Randomization**: Carefully modifies object class data while preserving critical flags
- **Non-destructive**: Creates new files without modifying originals
- **SCUMM v5 Support**: Specifically designed for games like Monkey Island 2

## Directory Structure

```
randomizer/
├── input/
│   └── MONKEY2/          # Original game files
│       ├── MONKEY2.000   # Index file
│       ├── MONKEY2.001   # Data file
│       └── ...           # Other game files
├── output/               # Generated randomized files
├── scumm_randomizer.h    # Randomizer class header
├── scumm_randomizer.cpp  # Randomizer implementation
├── scumm_randomizer_tool.cpp  # Main entry point
├── Makefile              # Build system
├── test_randomizer.sh    # Test script
└── README.md            # This file
```

## Building

1. Ensure you're in the randomizer directory:
   ```bash
   cd /home/slyryd/scummvm/randomizer
   ```

2. Build the tool:
   ```bash
   make all
   ```

3. (Optional) Run the test:
   ```bash
   make test
   ```

## Usage

### Basic Usage
```bash
./scumm_randomizer_tool MONKEY2
```

### Custom Directories
```bash
./scumm_randomizer_tool MONKEY2 /path/to/input /path/to/output
```

### Command Line Options
- `game_id`: SCUMM game identifier (e.g., MONKEY2, INDY3, LOOM)
- `input_dir`: Directory containing original .000/.001 files (optional)
- `output_dir`: Directory to write randomized files (optional)

## Testing with ScummVM

1. **Backup Original Files**: Always backup your original game files first!

2. **Copy Randomized Files**: Copy the generated files from `output/` to your ScummVM game directory

3. **Run ScummVM**: Start the game and observe the changes:
   - Objects may belong to different characters/rooms
   - Object states may be different (doors open/closed, items visible/hidden)
   - Game behavior may be significantly altered

## Technical Details

### What Gets Randomized

1. **Object Ownership** (`_objectOwnerTable`):
   - Which room or character owns each object
   - Affects item pickup, visibility, and interactions

2. **Object States** (`_objectStateTable`):
   - Object state flags (visible, locked, open, etc.)
   - Affects object appearance and behavior

3. **Object Classes** (`_classData`):
   - Object classification and properties
   - Carefully modified to preserve critical functionality

### What Doesn't Get Randomized

- Room layouts and graphics (in .001 data file)
- Scripts and game logic
- Sound and music data
- Core game mechanics

### SCUMM File Format

The tool understands these SCUMM index file blocks:
- `DOBJ`: Global object data (randomized)
- `DROO/DIRR`: Room resource list (copied)
- `DSCR/DIRS`: Script resource list (copied)
- `DCOS/DIRC`: Costume resource list (copied)
- `DSOU/DIRN`: Sound resource list (copied)
- `MAXS`: Resource limits (copied)
- `RNAM`: Room names (copied)

## Supported Games

Currently designed for SCUMM v5 games, particularly:
- **Monkey Island 2: LeChuck's Revenge** (primary target)

Can potentially be extended to support:
- Indiana Jones and the Fate of Atlantis
- Day of the Tentacle
- Sam & Max Hit the Road
- Other SCUMM v5/v6 games

## Troubleshooting

### Build Issues
- Ensure you have g++ and make installed
- Check that ScummVM source code is available in the parent directory
- Verify all header files are accessible

### Runtime Issues
- Ensure input .000/.001 files exist and are readable
- Check that output directory is writable
- Verify the game files are from a supported SCUMM version

### Game Issues
- If the randomized game crashes, try a different randomization seed
- Some object combinations may create unwinnable states
- Backup saves before testing randomized files

## Implementation Notes

The randomizer is built using ScummVM's own resource reading code, ensuring compatibility with the original file formats. It reads the same data structures that ScummVM uses during gameplay, modifies them according to randomization rules, and writes them back in the same format.

### Key Classes
- `ScummRandomizer`: Main class handling file I/O and randomization
- Object data structures match ScummVM's internal representations
- Randomization uses ScummVM's `RandomSource` for reproducible results

## Future Enhancements

Potential improvements:
- Support for more SCUMM versions
- GUI interface
- More granular randomization options
- Room layout randomization
- Script/dialog randomization
- Savegame compatibility

## License

This tool follows the same GPL license as the ScummVM project.

## Dev Notes

make -j32 && make randomizer -j32
