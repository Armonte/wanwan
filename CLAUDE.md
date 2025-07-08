# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a rollback netcode implementation for Fighter Maker 2nd (FM2K) games. The project consists of:

1. **FM2K_RollbackLauncher** - Main launcher application that manages game instances and networking
2. **FM2KHook** - DLL that gets injected into FM2K games to provide rollback functionality
3. **Comprehensive research documentation** - Detailed reverse engineering of FM2K's internals

## Build System

### Prerequisites
- MinGW-w64 cross-compiler (i686-w64-mingw32-gcc/g++)
- CMake 3.20+
- Ninja build system

### Build Commands
```bash
# Configure and build (run from project root)
./make_build.sh  # Sets up build environment and configures CMake
./go.sh          # Builds the project and copies outputs to /mnt/c/games/
```

### Build Outputs
- `FM2K_RollbackLauncher.exe` - Main application
- `FM2KHook.dll` - Hook DLL for injection into FM2K games

## Project Architecture

### Launcher Architecture (like LilithPort)
The project uses a proven launcher-based architecture:
1. User runs the rollback launcher
2. Launcher displays game selection UI
3. User selects FM2K game and network settings
4. Launcher creates FM2K game process in suspended state
5. Launcher injects FM2KHook.dll into the game process
6. Game resumes with rollback netcode active

### Key Components

#### Core Classes
- **FM2KGameInstance** (`FM2K_GameInstance.cpp`) - Manages FM2K game process lifecycle
- **NetworkSession** (`FM2K_NetworkSession.cpp`) - Handles GekkoNet networking
- **LauncherUI** (`FM2K_LauncherUI.cpp`) - ImGui-based launcher interface
- **FM2KLauncher** (`FM2K_RollbackClient.cpp`) - Main application controller

#### Hook System
- **FM2KHook** (`FM2KHook/`) - DLL injected into FM2K games
- **IPC** (`FM2KHook/src/ipc.cpp`) - Inter-process communication with launcher
- **StateManager** (`FM2KHook/src/state_manager.cpp`) - Game state capture/restore

#### Integration Layer
- **FM2K_Integration.h** - Complete FM2K memory layout and API definitions
- **FM2K_Hooks.cpp** - Hook implementations for game functions
- **FM2K_DLLInjector.cpp** - Process injection utilities

### Technical Details

#### FM2K Game Engine
- **Timing**: 100 FPS (10ms per frame)
- **Input System**: 11-bit input mask with 1024-frame history
- **State Size**: ~400KB per frame (optimized for rollback)
- **Memory Layout**: Well-documented addresses in FM2K_Integration.h

#### Networking
- **GekkoNet**: Proven rollback networking library
- **Input Prediction**: 3-frame window for smooth gameplay
- **Desync Detection**: Real-time Fletcher32 checksum validation

#### Cross-Compilation
- **Target**: Windows 32-bit (i686-w64-mingw32)
- **Host**: Linux/WSL environment
- **Static Linking**: All dependencies bundled for distribution

## Development Guidelines

### Code Organization
- Main application code in project root
- Hook DLL code in `FM2KHook/` subdirectory
- Vendored dependencies in `vendored/` (SDL3, GekkoNet, ImGui, MinHook)
- Documentation in `docs/` directory

### Key Memory Addresses
All critical FM2K memory addresses are documented in `FM2K_Integration.h`:
- Input buffers, game state, player health, timers
- Hook points for frame processing and state updates
- Effect system addresses for visual state

### Testing
- Place built executables in FM2K game directory
- Test with WonderfulWorld or other FM2K games
- Verify process injection and hook initialization

## Important Notes

### Security Considerations
- Process injection requires Administrator privileges
- Hook DLL must be 32-bit to match FM2K architecture
- Windows Defender may flag injection tools as suspicious

### Research Documentation
Extensive reverse engineering documentation is available in `docs/`:
- `FM2K_Rollback_Research.md` - Complete engine analysis
- `FM2K_Hook_Implementation.md` - Hook system details
- `outline/` directory - Detailed breakdowns of all game systems

### Cross-Platform Limitations
- Windows-only due to process injection requirements
- Uses Windows-specific APIs (CreateProcess, DLL injection)
- Could potentially be ported using different integration methods

## Claude Guidance

### Tool Usage Guidelines
- Never compile or run build scripts, that is not for you.
- Always try to reference our documentation and cross reference if possible for certainty.
- We have IDA MCP tools connected so you can scan the binary at offsets and read disasm and be more informed