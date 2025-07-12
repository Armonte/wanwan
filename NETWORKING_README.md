# FM2K GekkoNet Networking Implementation

This implementation follows the `OnlineSession_gekkonet_sdl2_example.cpp` pattern to provide rollback netcode for FM2K using GekkoNet with real UDP networking.

## Overview

The networking system consists of:
- **DLL Hook** (`FM2KHook/src/dllmain.cpp`): Injected into the game process to handle GekkoNet integration
- **Game Instance** (`FM2K_GameInstance.cpp`): Manages game process launch and networking configuration
- **Test Launcher** (`test_networking.cpp`): Demonstrates how to launch networked games

## Key Features

### 1. Real UDP Networking
- Uses GekkoNet's real UDP network adapter (not fake network)
- Configurable ports and remote addresses
- Automatic connection management

### 2. Adaptive Frame Timing
- Follows OnlineSession example's frame timing system
- Adjusts frame rate based on network conditions:
  - 58 FPS when ahead (slow down)
  - 60 FPS normal
  - 62 FPS when behind (speed up)

### 3. Input Prediction and Rollback
- Captures FM2K inputs and converts to GekkoNet format
- Applies predicted inputs to game memory
- Handles rollback events for desync correction

### 4. State Management
- Comprehensive state save/load system
- Automatic state saving for rollback buffer
- Manual save/load slots for debugging

## Usage

### Command Line Interface (like OnlineSession)

```bash
# Launch Player 0 (Host) on port 7000, connecting to Player 1 on port 7001
./test_networking.exe 0 7000 127.0.0.1:7001

# Launch Player 1 (Guest) on port 7001, connecting to Player 0 on port 7000
./test_networking.exe 1 7001 127.0.0.1:7000
```

### Programmatic Usage

```cpp
#include "FM2K_GameInstance.h"

// Create game instance
FM2KGameInstance game_instance;
game_instance.Initialize();

// Launch with networking (Player 0, port 7000, connecting to 127.0.0.1:7001)
game_instance.LaunchWithNetworking(0, 7000, "127.0.0.1:7001", "C:\\Games\\FM2K\\fm2k.exe");

// Monitor the game
while (game_instance.IsRunning()) {
    game_instance.ProcessDLLEvents();
    SDL_Delay(100);
}
```

## Implementation Details

### DLL Hook Integration

The DLL hook (`Hook_ProcessGameInputs`) follows the OnlineSession game loop pattern:

1. **Frame Timing**: Uses `gekko_frames_ahead()` to determine network conditions
2. **Network Polling**: Calls `gekko_network_poll()` every frame
3. **Input Processing**: Captures FM2K inputs and adds to GekkoNet session
4. **Event Handling**: Processes GekkoNet events (AdvanceEvent, SaveEvent, LoadEvent)
5. **State Management**: Saves/loads states as requested by GekkoNet

### Input Conversion

FM2K uses 11-bit inputs, converted to 8-bit GekkoNet format:

```cpp
// FM2K to GekkoNet
uint8_t ConvertFM2KInputToGekko(uint32_t fm2k_input) {
    uint8_t gekko_input = 0;
    if (fm2k_input & 0x01) gekko_input |= 0x01;  // left
    if (fm2k_input & 0x02) gekko_input |= 0x02;  // right
    if (fm2k_input & 0x04) gekko_input |= 0x04;  // up
    if (fm2k_input & 0x08) gekko_input |= 0x08;  // down
    if (fm2k_input & 0x10) gekko_input |= 0x10;  // button1
    if (fm2k_input & 0x20) gekko_input |= 0x20;  // button2
    if (fm2k_input & 0x40) gekko_input |= 0x40;  // button3
    if (fm2k_input & 0x80) gekko_input |= 0x80;  // button4
    return gekko_input;
}
```

### Environment Variables

The DLL reads networking configuration from environment variables:

- `FM2K_PLAYER_INDEX`: Player index (0 or 1)
- `FM2K_LOCAL_PORT`: Local UDP port
- `FM2K_REMOTE_ADDR`: Remote address (e.g., "127.0.0.1:7001")

### GekkoNet Configuration

```cpp
GekkoConfig config;
config.num_players = 2;
config.max_spectators = 0;
config.input_prediction_window = 10;  // 10-frame prediction window
config.spectator_delay = 0;
config.input_size = sizeof(uint8_t);  // 1 byte per input
config.state_size = sizeof(FM2K::State::GameState);
config.limited_saving = false;
config.post_sync_joining = false;
config.desync_detection = true;
```

## Testing

### Local Testing

1. **Terminal 1**: Launch Player 0
   ```bash
   ./test_networking.exe 0 7000 127.0.0.1:7001
   ```

2. **Terminal 2**: Launch Player 1
   ```bash
   ./test_networking.exe 1 7001 127.0.0.1:7000
   ```

### Network Testing

For testing over the internet, replace `127.0.0.1` with the actual IP address of the remote machine.

## Debugging

### Log Files

The DLL creates log files in `C:\Games\`:
- `fm2k_hook_host.txt` for Player 0
- `fm2k_hook_client.txt` for Player 1

### Console Output

The test launcher provides real-time status updates:
- Network statistics (ping, jitter, frames ahead)
- Input processing status
- GekkoNet event logging

### Shared Memory

The DLL communicates with the launcher via shared memory for:
- Network configuration updates
- Debug commands (save/load state)
- Performance statistics

## Troubleshooting

### Common Issues

1. **Port Already in Use**: Change the port numbers in the command line
2. **Firewall Blocking**: Allow the application through Windows Firewall
3. **DLL Injection Failed**: Ensure the game executable path is correct
4. **Network Timeout**: Check network connectivity and firewall settings

### Debug Commands

The shared memory interface supports debug commands:
- Manual state save/load
- Force rollback
- Auto-save configuration
- Performance monitoring

## Performance Considerations

- **State Size**: The current implementation uses ~850KB states (COMPLETE profile)
- **Frame Rate**: Adaptive timing maintains 60 FPS nominal
- **Network Bandwidth**: ~1-2 KB per frame for inputs + periodic state sync
- **Memory Usage**: Rollback buffer holds 8 frames by default

## Future Improvements

1. **State Compression**: Implement state compression to reduce bandwidth
2. **Spectator Mode**: Add spectator support for tournament streaming
3. **Replay System**: Integrate with existing replay system
4. **Cross-Platform**: Extend to Linux/macOS if needed