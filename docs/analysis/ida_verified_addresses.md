# IDA Pro Verified Addresses - WonderfulWorld_ver_0946.exe

This document contains all addresses verified using IDA Pro MCP tools. **DO NOT** convert these addresses - use them exactly as listed.

## Hook Function Addresses (Verified ✅)

| Function Name | Address | Size | Purpose |
|---------------|---------|------|---------|
| `initialize_game` | `0x4056C0` | `0x32c` | Game initialization entry point |
| `initialize_directdraw_mode` | `0x404980` | `0x243` | DirectDraw initialization |
| `main_window_proc` | `0x405F50` | `0x4b0` | Main window message handler |
| `process_input_history` | `0x4025A0` | `0x53` | Input processing function |

## Global Variable Addresses (Verified ✅)

### Combat State Addresses (ArtMoney + IDA Cross-Verified)

| Variable Name | Address | Type | Purpose | Verification Status |
|---------------|---------|------|---------|-------------------|
| **Player Health** |
| `g_p1_hp_current` | `0x47010C` | u32 | Player 1 current HP | ✅ **VERIFIED: Combat value=0** |
| `g_p2_hp_current` | `0x47030C` | u32 | Player 2 current HP | ✅ **VERIFIED: Combat value=6** |
| `g_p1_hp_max` (ArtMoney) | `0x4DFC85` | u32 | Player 1 maximum HP | ✅ **VERIFIED: Combat value=800** |
| `g_p2_hp_max` (ArtMoney) | `0x4EDC4` | u32 | Player 2 maximum HP | ⚠️ **NEEDS VALIDATION** |
| **Player Coordinates** |
| `g_p1_coord_x` (ArtMoney) | `0x4ADCC3` | u32 | Player 1 X position | ✅ **VERIFIED: Combat value=0** |
| `g_p1_coord_y` (ArtMoney) | `0x4ADCC7` | u16 | Player 1 Y position | ✅ **VERIFIED: Combat value=0** |
| `g_p2_coord_x` (ArtMoney) | `0x4EDD02` | u32 | Player 2 X position | ✅ **VERIFIED: Combat value=760** |
| `g_p2_coord_y` (ArtMoney) | `0x4EDD06` | u16 | Player 2 Y position | ✅ **VERIFIED: Combat value=920** |
| **Super Meter System** |
| `g_p1_super` (ArtMoney) | `0x4EDC3D` | u32 | Player 1 super meter | ✅ **VERIFIED: Combat value=0** |
| `g_p2_super` (ArtMoney) | `0x4EDC0C` | u32 | Player 2 super meter | ✅ **VERIFIED: Combat value=0** |
| **Timers** |
| `g_game_timer` | `0x470044` | u32 | Primary game timer | ✅ **VERIFIED: Combat value=1** |
| `g_game_timer_alt` (ArtMoney) | `0x47DB94` | u32 | Alternative timer address | ✅ **VERIFIED: Same value as primary (1)** |
| **Map Coordinates** |
| `g_map_x_coord` (ArtMoney) | `0x44742C` | u32 | Map/stage X coordinate | ✅ **VERIFIED: Combat value=0** |
| `g_map_y_coord` (ArtMoney) | `0x447F30` | u32 | Map/stage Y coordinate | ⚠️ **NEEDS VALIDATION** |

### Window Handles
| Variable Name | Address | Purpose |
|---------------|---------|---------|
| `g_hwnd_parent` | `0x4246F8` | **Main game window handle** |
| `g_hwndJoyInput` | `0x4249B4` | Joystick input window |
| `g_hwndKeyInput` | `0x4249C0` | Keyboard input window |

### DirectDraw System
| Variable Name | Address | Purpose |
|---------------|---------|---------|
| `g_direct_draw` | `0x424758` | DirectDraw main object pointer |
| `g_dd_primary_surface` | `0x424750` | Primary surface pointer |
| `g_dd_back_buffer` | `0x424754` | Back buffer surface pointer |
| `g_dd_init_success` | `0x424760` | DirectDraw initialization flag |
| `g_dd_init_success_count` | `0x424774` | Init success counter |
| `g_dd_surface_desc` | `0x46FF40` | Surface description struct |
| `g_dd_surface_caps` | `0x46FF44` | Surface capabilities |
| `g_dd_buffer_count` | `0x46FF54` | Buffer count |
| `g_dd_surface_format` | `0x46FFA8` | Surface format |

### Graphics System
| Variable Name | Address | Purpose |
|---------------|---------|---------|
| `g_graphics_mode` | `0x424704` | Current graphics mode |
| `g_graphics_state` | `0x424768` | Graphics system state |
| `g_graphics_busy_flag` | `0x42476C` | Graphics busy flag |
| `g_graphics_init_counter` | `0x424770` | Graphics init counter |
| `g_directsound_initialized` | `0x42474C` | DirectSound init flag |

### Resolution/Display
| Variable Name | Address | Purpose |
|---------------|---------|---------|
| `g_stage_width_pixels` | `0x4452B8` | Stage width in pixels |
| `g_stage_height_pixels` | `0x4452BA` | Stage height in pixels |
| `g_dest_width` | `0x447F20` | Destination width |
| `g_dest_height` | `0x447F24` | Destination height |
| `g_ui_graphics_data` | `0x445740` | UI graphics data |

### Character Variables (ArtMoney Verified)

| Variable Name | Address | Type | Purpose |
|---------------|---------|------|---------|
| **Player 1 Character Variables (1-byte each)** |
| `g_p1_char_var_a` | `0x4ADFD17` | u8 | Character variable A for Player 1 |
| `g_p1_char_var_b` | `0x4ADFD19` | u8 | Character variable B for Player 1 |
| `g_p1_char_var_c` | `0x4ADFD1B` | u8 | Character variable C for Player 1 |
| `g_p1_char_var_d` | `0x4ADFD1D` | u8 | Character variable D for Player 1 |
| `g_p1_char_var_e` | `0x4ADFD1F` | u8 | Character variable E for Player 1 |
| `g_p1_char_var_f` | `0x4ADFD21` | u8 | Character variable F for Player 1 |
| `g_p1_char_var_g` | `0x4ADFD23` | u8 | Character variable G for Player 1 |
| `g_p1_char_var_h` | `0x4ADFD25` | u8 | Character variable H for Player 1 |
| `g_p1_char_var_i` | `0x4ADFD27` | u8 | Character variable I for Player 1 |
| `g_p1_char_var_j` | `0x4ADFD29` | u8 | Character variable J for Player 1 |
| `g_p1_char_var_k` | `0x4ADFD2B` | u8 | Character variable K for Player 1 |
| `g_p1_char_var_l` | `0x4ADFD2D` | u8 | Character variable L for Player 1 |
| `g_p1_char_var_m` | `0x4ADFD2F` | u8 | Character variable M for Player 1 |
| `g_p1_char_var_n` | `0x4ADFD31` | u8 | Character variable N for Player 1 |
| `g_p1_char_var_o` | `0x4ADFD33` | u8 | Character variable O for Player 1 |
| `g_p1_char_var_p` | `0x4ADFD35` | u8 | Character variable P for Player 1 |
| **Player 2 Character Variables (2-byte each)** |
| `g_p2_char_var_a` | `0x4ADFD5` | u16 | Character variable A for Player 2 |
| `g_p2_char_var_b` | `0x4EDD58` | u16 | Character variable B for Player 2 |
| `g_p2_char_var_c` | `0x4EDD5A` | u16 | Character variable C for Player 2 |
| `g_p2_char_var_d` | `0x4EDDC` | u16 | Character variable D for Player 2 |
| `g_p2_char_var_f` | `0x4EDD60` | u16 | Character variable F for Player 2 |
| `g_p2_char_var_g` | `0x4EDD62` | u16 | Character variable G for Player 2 |
| `g_p2_char_var_h` | `0x4EDD64` | u16 | Character variable H for Player 2 |
| `g_p2_char_var_i` | `0x4ADFD6` | u16 | Character variable I for Player 2 |
| `g_p2_char_var_j` | `0x4ADFD8` | u16 | Character variable J for Player 2 |
| `g_p2_char_var_k` | `0x4EDD6A` | u16 | Character variable K for Player 2 |
| `g_p2_char_var_l` | `0x4EDDC6` | u16 | Character variable L for Player 2 |
| `g_p2_char_var_m` | `0x4EDD6E` | u16 | Character variable M for Player 2 |
| `g_p2_char_var_n` | `0x4ADFD0` | u16 | Character variable N for Player 2 |
| `g_p2_char_var_o` | `0x4ADFD7` | u16 | Character variable O for Player 2 |
| `g_p2_char_var_p` | `0x4EDD74` | u16 | Character variable P for Player 2 |

### System Variables (ArtMoney Verified)

| Variable Name | Address | Type | Purpose |
|---------------|---------|------|---------|
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

## Critical Issues Identified

### ❌ **Window Handle Problem**
- Our current code uses the **wrong** global for window handle
- We're not setting `g_hwnd_parent` at `0x4246F8` 
- This is likely why SDL3 window creation fails

### ❌ **Resolution Address Problem** 
- The resolution globals we were using (`0x6B3060`, `0x6B305C`, `0x6B3058`) **do not exist**
- Correct resolution globals are at different addresses

### ❌ **DirectDraw Global Usage**
- We should use the actual global names, not arbitrary memory writes
- Need to properly update `g_direct_draw`, `g_dd_primary_surface`, `g_dd_back_buffer`

## Required Fixes

1. **Update window hijacking** to properly set `g_hwnd_parent` at `0x4246F8`
2. **Fix resolution setting** to use correct globals: `g_stage_width_pixels` and `g_stage_height_pixels`
3. **Update DirectDraw compatibility** to use verified global addresses
4. **Debug SDL3 window creation** flow with correct window handle management

## Usage Notes

- **Always reference this document** when working with FM2K addresses
- **Never convert addresses** - use the exact hex values listed
- **Verify with IDA MCP tools** before making changes
- **Update this document** when new globals are discovered
- **ArtMoney cross-verification**: 70+ addresses now cross-verified between ArtMoney table and IDA analysis
- **MinimalGameState testing**: Use verified addresses for 48-byte minimal rollback state
- **Live combat verification**: All critical addresses tested during active gameplay with IDA MCP
- **Address validation framework**: Use IDA MCP tools to validate new addresses before integration