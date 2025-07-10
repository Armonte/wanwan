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