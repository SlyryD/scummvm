# Randomizer

This tool reads game files, randomizes game items and entrances, and writes modified files that can be used with ScummVM, any other emulator, or original hardware.

## Features

- **Item randomizer**: Picking up an item gives the player a random item
- **Entrance randomier**: Not yet supported: entering a new area brings you to a random area
- **Logical randomization**: Preserves game logic to ensure the game is completable
- **Non-destructive**: Creates new files without modifying originals
- **SCUMM v5 support**: Specifically designed for games like Monkey Island 2
- **Archipelago integration**: Not yet supported: Randomizers work with [Archipelago](https://archipelago.gg/)

## Philosophy

Rather than modify the emulators running the games, it's better to modify the game files. This allows the randomized game files to be used on any platform (e.g., original hardware) and doesn't require specific emulators. That way, randomizer players can use their preferred versions of the games on their preferred hardware/emulators.

Randomizers can be created by tapping into each engine. For example, when the RANDOMIZER macro is defined, in `ScummEngine::run()`, instead of calling `ScummEngine::go()` to run the game, it will call `ScummEngine::randomizeGameFiles()`. The idea here is that all the context needed for running the game will be loaded, making it easier to randomize game files. This might be a faulty assumption, but it's what I'm going with now.

Logic files should be a consistent format across all games, so the same randomizer code can be used for all games. The randomizer will use the logic to ensure that the game is still completable. For example, if a key is required to open a door, the randomizer will ensure that the key is placed somewhere accessible before the door.

## Directory Structure

```
randomizer/
├── input/
│   └── MONKEY2/          # Original game files
│       ├── MONKEY2.000   # Index file
│       ├── MONKEY2.001   # Data file
│       └── ...           # Other game files
├── output/               # Generated randomized files
│   └── MONKEY2/          # Modified game files
│       ├── MONKEY2.000   # Modified index file
│       ├── MONKEY2.001   # Randomized data file
│       └── ...           # Other game files
├── AGENTS.md             # This file
├── randomizer            # Randomizer executable
└── randomizer.dwp        # Randomizer workspace file
```

## Building

At the root of the repository, run

```bash
make randomizer
```

## Cleaning

At the root of the repository, run

```bash
make randomizerclean
```

## Usage

### Basic Usage
```bash
./randomizer --path=./input/<game id> <game id>
```

## Technical Details

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
I think we need to move whole object files (OBIM and OBCD) except for the pickup VERB script

