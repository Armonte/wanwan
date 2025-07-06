# FM2K Rollback Netcode Client

A modern rollback netcode implementation for Fighter Maker 2nd (FM2K) games, built with SDL3 and GekkoNet.

## Architecture: Launcher-Based (Like LilithPort)

This client uses the **correct** launcher-based architecture:

1. **User starts the rollback client**
2. **Client shows game selection UI**
3. **User selects FM2K game and network settings**
4. **Client launches the FM2K game with hooks**
5. **Client manages networking while game runs**
6. **When game ends, client can launch another match**

This is the same pattern used by LilithPort and other successful rollback implementations.

## Key Features

- **100 FPS rollback netcode** optimized for FM2K's timing
- **Complete state management** using exact memory addresses
- **Modern SDL3 + GekkoNet architecture**
- **Process injection with MinHook** for seamless integration
- **Comprehensive desync detection** with Fletcher32 checksums
- **Input prediction** with 3-frame window (30ms)
- **Automatic game discovery** (.kgt/.exe file detection)

## Usage

### Quick Start
```bash
# Place the client in your FM2K game folder
# (same directory as your .kgt and .exe files)
./fm2k_rollback_client.exe 0 7000 127.0.0.1:7001 2
```

### Command Line Arguments
```
fm2k_rollback_client.exe <local_player> <local_port> <remote_address> <local_delay>

local_player:    0 or 1 (which player you are)
local_port:      Your port number (e.g., 7000)
remote_address:  Opponent's IP:port (e.g., 192.168.1.100:7001)
local_delay:     Input delay in frames (2-4 recommended)
```

### Example Sessions
```bash
# Player 1 (host)
./fm2k_rollback_client.exe 0 7000 192.168.1.100:7001 2

# Player 2 (client)  
./fm2k_rollback_client.exe 1 7001 192.168.1.50:7000 2
```

## Technical Details

### FM2K Integration
- **Memory addresses**: Based on comprehensive reverse engineering
- **State size**: ~400KB per frame (optimized for rollback)
- **Input system**: 11-bit input mask with 1024-frame history
- **Timing**: 100 FPS (10ms per frame) with accumulator-based timing
- **RNG**: Deterministic Linear Congruential Generator state capture

### Network Architecture
- **GekkoNet**: Proven rollback networking library
- **Input prediction**: 3-frame window for smooth gameplay
- **Desync detection**: Real-time checksum validation
- **Spectator support**: Up to 8 spectators per match
- **Connection recovery**: Automatic reconnection on network issues

### Performance Optimizations
- **Suspended launch**: Game starts paused for hook setup
- **Memory-mapped I/O**: Direct process memory access
- **Efficient state serialization**: Only essential game state
- **Frame-perfect timing**: Accumulator prevents drift

## File Structure

```
FM2K_RollbackClient/
„¥„Ÿ„Ÿ FM2K_RollbackClient.cpp    # Main launcher and rollback logic
„¥„Ÿ„Ÿ FM2K_Integration.h         # Complete FM2K integration API
„¥„Ÿ„Ÿ CMakeLists.txt            # Build configuration
„¥„Ÿ„Ÿ vendored/                 # Dependencies
„    „¥„Ÿ„Ÿ GekkoNet/            # Rollback networking
„    „¥„Ÿ„Ÿ minhook/             # Process injection
„    „¤„Ÿ„Ÿ SDL/                 # Modern SDL3
„¤„Ÿ„Ÿ docs/                    # Research and documentation
    „¥„Ÿ„Ÿ FM2K_Rollback_Research.md
    „¤„Ÿ„Ÿ LilithPort_SDL3_Migration_Strategy.md
```

## Building

### Prerequisites
- **Windows 10/11** (required for process injection)
- **Visual Studio 2022** or **MinGW-w64**
- **CMake 3.20+**
- **Git** (for vendored dependencies)

### Build Steps
```bash
git clone --recursive https://github.com/yourusername/FM2K_RollbackClient.git
cd FM2K_RollbackClient
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### Output
- Executable: `build/bin/fm2k_rollback_client.exe`
- Place in your FM2K game directory for automatic game detection

## Game Compatibility

### Supported Games
- **WonderfulWorld** (tested)
- **Any FM2K game** with standard memory layout
- **Custom FM2K games** (may require address adjustments)

### Game Detection
The client automatically detects FM2K games by:
1. Looking for `.kgt` files with matching `.exe` files
2. Scanning for common FM2K executable names
3. Validating executable format and compatibility

## Troubleshooting

### Common Issues

**"No FM2K game found"**
- Ensure the client is in the same directory as your FM2K game files
- Check that both `.kgt` and `.exe` files exist
- Verify the executable is a valid FM2K game

**"Failed to launch FM2K game"**
- Run as Administrator (required for process injection)
- Check Windows Defender/antivirus settings
- Ensure the game executable is not corrupted

**"Connection failed"**
- Verify both players are using the same client version
- Check firewall settings (allow the client through)
- Ensure correct IP addresses and ports
- Try local loopback (127.0.0.1) first

**"Desync detected"**
- Both players must use identical game files
- Check for different game versions or modifications
- Verify stable network connection
- Try increasing local delay (3-4 frames)

### Performance Tips
- **Close unnecessary programs** for consistent timing
- **Use wired network connection** for stability
- **Set game to high priority** in Task Manager
- **Disable Windows Game Mode** for consistent performance

## Research Documentation

This implementation is based on comprehensive reverse engineering research:

- **FM2K_Rollback_Research.md**: Complete memory layout and engine analysis
- **LilithPort_SDL3_Migration_Strategy.md**: Architecture decisions and migration strategy

## Contributing

1. Fork the repository
2. Create a feature branch
3. Test with multiple FM2K games
4. Submit a pull request with detailed description

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Acknowledgments

- **LilithPort** for demonstrating the correct launcher architecture
- **GekkoNet** for robust rollback networking
- **SDL3** for modern cross-platform development
- **FM2K community** for game preservation and research

---

**Note**: This client requires Windows due to process injection limitations. Future versions may support other platforms through different integration methods. 