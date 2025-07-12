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
| Global Name | Address | Description |
| :--- | :--- | :--- |
| `g_game_timer` | `0x470044` | The main round timer that counts down during a match. |
| `g_round_timer` | `0x470060` | Initialized with the round duration from the game's config files. |
| `g_round_timer_counter` | `0x424F00` | A secondary timer or counter used for specific game state transitions. |

### Match State
| Global Name | Address | Description |
| :--- | :--- | :--- |
| `g_game_state_flag`| `0xDFC6D` (offset) | A central flag that controls the overall game loop and state transitions. This appears to be an offset within a larger, unidentified data structure. |
| `g_game_mode` | `0x470054` | Defines the current mode of play (e.g., character select, in-game, versus). |
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

| Global Name | Address | Description |
| :--- | :--- | :--- |
| `g_timer_countdown1` | `0x4456E4` | A general-purpose countdown timer. |
| `g_timer_countdown2` | `0x447D91` | A second general-purpose countdown timer. |
| `unk_4DFDA7` | `0x4DFDA7` (P1) | A per-player timer or cooldown, decremented each frame. Offset from player slot is `+0xE027`. |

--- 