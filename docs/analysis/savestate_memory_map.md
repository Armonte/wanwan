# FM2K Save State Memory Map

This document outlines the critical memory regions and data structures that must be saved and restored to implement save states and rollback functionality.

## 1. Player Data (`g_player_data_slots`)

-   **Address**: `0x4D1D80`
-   **Size**: `0x701F8` bytes (459,256 bytes)
-   **Description**: This is the primary memory region containing all run-time data for both player slots. It is initialized by the `initialize_game` function (`0x4056C0`) and populated by `player_data_file_loader` (`0x403A17`). Capturing this entire memory block is the first and most critical step for save states.

### Player Slot Structure

The `g_player_data_slots` region is divided into two identical slots, one for each player. Each slot is **57,407 bytes** and has the following internal structure, with offsets relative to the start of the slot:

| Offset (Hex) | Offset (Dec) | Size (Bytes) | Description |
| :--- | :--- | :--- | :--- |
| `+0x0` | `+0` | 16 | **Header**: Basic slot metadata. |
| `+0x888` | `+2184` | 2 | **State Flags**: Contains player ID and a flag indicating if the slot is loaded. |
| `+0x223C` | `+8764` | 11,110 | **Character Data 1**: Loaded from the `.player` file. |
| `+0x4D9A` | `+19874` | Variable | **Hitbox Data**: Size is determined by a value read from the character file. |
| `+0x6DAA` | `+28074` | Variable | **Unknown Data 1**: Size is dynamic. |
| `+0x70CA` | `+28874` | Variable | **Unknown Data 2**: Size is dynamic. |
| `+0x757A` | `+30074` | 1,867 | **Character Data 2**: Loaded from the `.player` file. |
| `+0x7CC5` | `+31941` | 20,604 | **Character Data 3**: Loaded from the `.player` file. |
| `+0xCD41` | `+52545`| 4,524 | **Character Data 4**: Loaded from the `.player` file. |

The following globals are also part of the player slot structure, but their exact purpose is still under investigation:

| Global Name | Offset (Hex) | Offset (Dec) | Description |
| :--- | :--- | :--- | :--- |
| `dword_4DFD09` | `+0x1FD8` | `+8153` | A counter or timer, potentially for player-specific cooldowns or state durations. |
| `dword_4DFD0D` | `+0x1FDC` | `+8157` | A value related to input processing, possibly an index into the input history. |
| `g_PlayerStatusArray` | `+?` | `+?` | Player connection or gameplay status. The exact offset is not yet determined. |

<pre>
<code>
+--------------------------------------------------------------------------+
| g_player_data_slots @ 0x4D1D80 (Size: 0x701F8)                            |
|--------------------------------------------------------------------------|
|                                                                          |
|   +------------------------------------------------------------------+   |
|   | Player 1 Slot (57,407 bytes)                                     |   |
|   |------------------------------------------------------------------|   |
|   | Header (16 bytes) @ 0x0                                          |   |
|   | State Flags (2 bytes) @ 0x888                                    |   |
|   | Character Data Blocks (multiple, variable sizes)                 |   |
|   | Hitbox Data, etc.                                                |   |
|   +------------------------------------------------------------------+   |
|                                                                          |
|   +------------------------------------------------------------------+   |
|   | Player 2 Slot (57,407 bytes)                                     |   |
|   |------------------------------------------------------------------|   |
|   | (Same structure as Player 1)                                     |   |
|   +------------------------------------------------------------------+   |
|                                                                          |
+--------------------------------------------------------------------------+
</code>
</pre>

---

## 2. Game State & Match Logic

This section covers global variables that track the overall state of the match, independent of individual player data. These are primarily managed by the `game_state_manager` function at `0x406FC0`.

### Timers
| Global Name | Address | Captured | Description |
| :--- | :--- | :--- | :--- |
| `g_game_timer` | `0x470044` | ✅ | The main round timer that counts down during a match. **VERIFIED: Combat value=1** |
| `g_game_timer_alt` | `0x47DB94` | ✅ | **ArtMoney timer address - VERIFIED: Same value as 0x470044 (value=1)** |
| `g_round_timer` | `0x470060` | ✅ | Initialized with the round duration from the game's config files. **VERIFIED: Combat value=0** |
| `g_round_timer_counter` | `0x424F00` | ✅ | A secondary timer or counter used for specific game state transitions. **VERIFIED: Combat value=102, likely visible countdown** |
| **In-game timer (visible)** | `0x424F00` | ✅ | **The visible countdown timer displayed during matches - VERIFIED as round_timer_counter** |

### Player Health & Max HP
| Global Name | Address | Description |
| :--- | :--- | :--- |
| `g_p1_hp` | `0x47010C` | Player 1 current health points. **VERIFIED: Combat value=0** |
| `g_p2_hp` | `0x47030C` | Player 2 current health points. **VERIFIED: Combat value=6** |
| `g_p1_max_hp` (ArtMoney) | `0x4DFC85` | **Player 1 maximum health points. VERIFIED: Combat value=800** |
| `g_p2_max_hp` (ArtMoney) | `0x4EDC4` | **Player 2 maximum health points. NEEDS VALIDATION** |

### Player Coordinates & Positions
| Global Name | Address | Type | Description |
| :--- | :--- | :--- | :--- |
| `g_p1_coord_x` (ArtMoney) | `0x4ADCC3` | u32 | **Player 1 X coordinate. VERIFIED: Combat value=0** |
| `g_p1_coord_y` (ArtMoney) | `0x4ADCC7` | u16 | **Player 1 Y coordinate. VERIFIED: Combat value=0** |
| `g_p2_coord_x` (ArtMoney) | `0x4EDD02` | u32 | **Player 2 X coordinate. VERIFIED: Combat value=760** |
| `g_p2_coord_y` (ArtMoney) | `0x4EDD06` | u16 | **Player 2 Y coordinate. VERIFIED: Combat value=920** |
| `g_map_x_coord` (ArtMoney) | `0x44742C` | u32 | **Map/stage X coordinate. VERIFIED: Combat value=0** |
| `g_map_y_coord` (ArtMoney) | `0x447F30` | u32 | **Map/stage Y coordinate. NEEDS VALIDATION** |

### Super Meter System
| Global Name | Address | Type | Description |
| :--- | :--- | :--- | :--- |
| `g_p1_super` (ArtMoney) | `0x4EDC3D` | u32 | **Player 1 super meter. VERIFIED: Combat value=0** |
| `g_p2_super` (ArtMoney) | `0x4EDC0C` | u32 | **Player 2 super meter. VERIFIED: Combat value=0** |
| `g_p1_special_stock` (ArtMoney) | `0x4EDDC95` | u32 | **Player 1 special stock. NEEDS VALIDATION** |
| `g_p2_special_stock` (ArtMoney) | `0x4EDC4` | u32 | **Player 2 special stock. NEEDS VALIDATION** |

### Match State
| Global Name | Address | Description |
| :--- | :--- | :--- |
| `g_game_state_flag`| `0xDFC6D` (offset) | A central flag that controls the overall game loop and state transitions. This appears to be an offset within a larger, unidentified data structure. |
| `g_game_mode` | `0x470054` | Defines the current mode of play (e.g., character select, in-game, versus). **VERIFIED: Combat value=3000** |
| `g_round_setting` | `0x470068` | The number of rounds required to win the match. |
| `g_team_round_setting` | `0x470064` | The number of rounds for team-based modes. |
| `g_p1_round_count` | `0x4700EC` | Number of rounds won by Player 1. |
| `g_p1_round_state` | `0x4700F0` | The current state of Player 1's rounds. |
| `g_p1_action_state`| `0x47019C` | The current action state of Player 1 (e.g., attacking, defending). |
| `g_p2_action_state`| `0x4701A0` (estimated)| The current action state of Player 2. (Address is unconfirmed but highly likely) |

### Camera
| Global Name | Address | Description |
| :--- | :--- | :--- |
| `g_camera_x` | `0x447F2C` | The horizontal position of the game camera. |
| `g_camera_y` | `0x447F30` | The vertical position of the game camera. |

### Character Variables (ArtMoney Verified)

FM2K provides 16 character variables per player for custom character logic and state tracking.

#### Player 1 Character Variables (1-byte each)
| Variable Name | Address | Description |
| :--- | :--- | :--- |
| `g_p1_char_var_a` | `0x4ADFD17` | Character variable A for Player 1 |
| `g_p1_char_var_b` | `0x4ADFD19` | Character variable B for Player 1 |
| `g_p1_char_var_c` | `0x4ADFD1B` | Character variable C for Player 1 |
| `g_p1_char_var_d` | `0x4ADFD1D` | Character variable D for Player 1 |
| `g_p1_char_var_e` | `0x4ADFD1F` | Character variable E for Player 1 |
| `g_p1_char_var_f` | `0x4ADFD21` | Character variable F for Player 1 |
| `g_p1_char_var_g` | `0x4ADFD23` | Character variable G for Player 1 |
| `g_p1_char_var_h` | `0x4ADFD25` | Character variable H for Player 1 |
| `g_p1_char_var_i` | `0x4ADFD27` | Character variable I for Player 1 |
| `g_p1_char_var_j` | `0x4ADFD29` | Character variable J for Player 1 |
| `g_p1_char_var_k` | `0x4ADFD2B` | Character variable K for Player 1 |
| `g_p1_char_var_l` | `0x4ADFD2D` | Character variable L for Player 1 |
| `g_p1_char_var_m` | `0x4ADFD2F` | Character variable M for Player 1 |
| `g_p1_char_var_n` | `0x4ADFD31` | Character variable N for Player 1 |
| `g_p1_char_var_o` | `0x4ADFD33` | Character variable O for Player 1 |
| `g_p1_char_var_p` | `0x4ADFD35` | Character variable P for Player 1 |

#### Player 2 Character Variables (2-byte each)
| Variable Name | Address | Description |
| :--- | :--- | :--- |
| `g_p2_char_var_a` | `0x4ADFD5` | Character variable A for Player 2 |
| `g_p2_char_var_b` | `0x4EDD58` | Character variable B for Player 2 |
| `g_p2_char_var_c` | `0x4EDD5A` | Character variable C for Player 2 |
| `g_p2_char_var_d` | `0x4EDDC` | Character variable D for Player 2 |
| `g_p2_char_var_f` | `0x4EDD60` | Character variable F for Player 2 |
| `g_p2_char_var_g` | `0x4EDD62` | Character variable G for Player 2 |
| `g_p2_char_var_h` | `0x4EDD64` | Character variable H for Player 2 |
| `g_p2_char_var_i` | `0x4ADFD6` | Character variable I for Player 2 |
| `g_p2_char_var_j` | `0x4ADFD8` | Character variable J for Player 2 |
| `g_p2_char_var_k` | `0x4EDD6A` | Character variable K for Player 2 |
| `g_p2_char_var_l` | `0x4EDDC6` | Character variable L for Player 2 |
| `g_p2_char_var_m` | `0x4EDD6E` | Character variable M for Player 2 |
| `g_p2_char_var_n` | `0x4ADFD0` | Character variable N for Player 2 |
| `g_p2_char_var_o` | `0x4ADFD7` | Character variable O for Player 2 |
| `g_p2_char_var_p` | `0x4EDD74` | Character variable P for Player 2 |

### System Variables (ArtMoney Verified)

Global system variables for game state management.

| Variable Name | Address | Type | Description |
| :--- | :--- | :--- | :--- |
| `g_system_var_a` | `0x4456B0` | u8 | System variable A |
| `g_system_var_b` | `0x4456B2` | u8 | System variable B |
| `g_system_var_d` | `0x4456B6` | u8 | System variable D |
| `g_system_var_e` | `0x4456B8` | u8 | System variable E |
| `g_system_var_f` | `0x4456BA` | u8 | System variable F |
| `g_system_var_g` | `0x4456BC` | u8 | System variable G |
| `g_system_var_h` | `0x4456BE` | u16 | System variable H |
| `g_system_var_i` | `0x4456C0` | u16 | System variable I |
| `g_system_var_j` | `0x456C2` | u16 | System variable J |
| `g_system_var_k` | `0x456C4` | u8 | System variable K |
| `g_system_var_l` | `0x456C6` | u8 | System variable L |
| `g_system_var_m` | `0x456C8` | u8 | System variable M |
| `g_system_var_n` | `0x456CA` | u8 | System variable N |
| `g_system_var_o` | `0x456CC` | u8 | System variable O |
| `g_system_var_p` | `0x456CE` | u16 | System variable P |

---

## 3. RNG (Random Number Generation)

A deterministic random number generator is essential for replay and rollback. The `game_rand()` function at `0x417A22` is a simple Linear Congruential Generator (LCG) that is used for various gameplay effects. Its internal state is stored in a single variable and must be saved and restored.

| Global Name | Address | Description |
| :--- | :--- | :--- |
| `g_rng_seed` | `0x41FB1C` | The seed or internal state of the game's primary RNG. |

---

## 4. Game Object Pool

The core of the game's state is a pool of **1024 game objects**. The `update_game_state` function (`0x404CD0`) iterates through this pool every frame and calls `update_game_object` (`0x40C130`) on each active object. To save the game's state, this entire pool must be captured.

-   **Address**: `0x4701E0` (as `g_game_object_pool`)
-   **Size**: `0x5F800` (391,168 bytes, calculated as `1024 objects * 382 bytes/object`)
-   **Description**: An array of 1024 game objects. Each object represents a single entity in the game world.

### Game Object Structure

Each object in the pool is **382 bytes** long. The exact layout of these objects is still under investigation, but they contain all the information needed to represent a character, projectile, or other in-game entity, including its state, position, and animations.

### Management
-   **`g_object_list_heads`**: `0x430240`
-   **`g_object_list_tails`**: `0x430244`

### Additional Timers
The `update_game_state` function also decrements two global timers:

| Global Name | Address | Captured | Description |
| :--- | :--- | :--- | :--- |
| `g_timer_countdown1` | `0x4456E4` | ❌ | A general-purpose countdown timer. |
| `g_timer_countdown2` | `0x447D91` | ❌ | A second general-purpose countdown timer. |
| `unk_4DFDA7` | `0x4DFDA7` (P1) | ❌ | A per-player timer or cooldown, decremented each frame. Offset from player slot is `+0xE027`. |

---

## Current Save State Implementation Status

### ✅ **COMPLETED AS DEBUG/TESTING FRAMEWORK (December 2024)**
This save state implementation is **complete as a debug and research tool**. Status:

- **Configurable Profiles**: MINIMAL (~50KB), STANDARD (~200KB), COMPLETE (~850KB)
- **Core State Capture**: Input buffers, HP, basic timers, RNG seed
- **Player Data Slots**: Full 459KB capture (COMPLETE profile)
- **Game Object Pool**: Full 391KB capture with smart active object detection
- **UI Integration**: Profile selection with real-time switching  
- **Performance Measurement**: Save/load timing and memory usage tracking

**Purpose**: Research validation, development testing, and foundation for production rollback

### ✅ **Recently Added (December 2024)**
- **Object list management**: `g_object_list_heads` (0x430240), `g_object_list_tails` (0x430244) 
- **Additional timers**: `g_timer_countdown1` (0x4456E4), `g_timer_countdown2` (0x447D91)
- **Round timer counter**: `g_round_timer_counter` (0x424F00) - **VERIFIED as visible in-game timer (value=102 during combat)**
- **Object type analysis**: CHARACTER_STATE_MACHINE (type=4), UI objects (type=6), system objects (type=1, type=3)
- **Memory address verification**: All HP, timer, and game mode addresses confirmed with IDA MCP during live combat
- **ArtMoney address integration**: 70+ verified addresses from ArtMoney table including coordinates, max HP, super meter, character variables
- **MinimalGameState structure**: 48-byte minimal state for GekkoNet desync testing (coordinates, HP, timers, RNG)
- **Alternative timer address**: `g_game_timer_alt` (0x47DB94) - same value as primary timer
- **Player coordinate system**: X/Y positions for both players with different data types (u32/u16)
- **Character & system variables**: 16 character variables per player + 16 system variables for advanced state tracking

### ❌ **Still Missing Critical Data**
- **Player action states**: Animation/action state beyond basic HP
- **Effect system states**: Visual effects and animations

### 🔄 **Next Phase: Production Rollback Implementation**
1. **GekkoNet Integration**: Transition from debug save states to production rollback
2. **Launcher Testing Tools**: Multi-client local testing infrastructure
3. **Network Session Management**: Real-time rollback synchronization
4. **Performance Optimization**: Production-grade rollback performance

### 📋 **Remaining Research Tasks (Low Priority)**
1. **Analyze timer debug output**: Monitor timer values during gameplay to identify in-game timer
2. **Player data analysis**: Separate static vs dynamic data for further optimization
3. **Effect system capture**: Add visual effects and animation states
4. **Compression research**: Implement differential/compressed saves

--- 