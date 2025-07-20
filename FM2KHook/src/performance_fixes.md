# FM2K Hook Performance Optimization Plan

## Major Performance Issues Identified:

### 1. **Object Scanning (HIGHEST IMPACT)**
- Currently disabled but still checking conditions
- When enabled, scans 1024 objects × 382 bytes = 390KB every frame
- Fix: Remove entirely or use frame skipping (every 30-60 frames)

### 2. **Excessive Logging**
- Multiple log calls per frame with string formatting
- Fix: Add production mode flag to disable most logging
- Keep only critical error logs

### 3. **CSS Sync Overhead**
- Reads 8+ memory locations every update
- Fix: Already reduced to every 5 frames, could go to every 10

### 4. **Game State Monitoring**
- Reads 3 memory addresses every frame
- Fix: Only check when expecting transitions (every 10-30 frames)

### 5. **Memory Safety Checks**
- IsBadReadPtr/IsBadWritePtr are expensive Windows APIs
- Fix: Cache validated addresses or use exception handling

## Quick Wins:

1. **Disable all object scanning during gameplay**
2. **Add FM2K_PRODUCTION_MODE=1 environment variable to disable logging**
3. **Reduce state monitoring frequency to every 30 frames**
4. **Cache memory validation results**
5. **Remove unused boot analysis code**

## Code Changes Needed:

1. In `Hook_ProcessGameInputs()`:
   - Remove lines 330-391 (SaveEvent object scanning)
   - Remove lines 406-425 (LoadEvent object restoration)
   - Reduce CSS update to every 10 frames
   - Add production mode checks around all logging

2. In `MonitorGameStateTransitions()`:
   - Add frame counter to only check every 30 frames
   - Cache last known good pointers

3. Global:
   - Add `if (!production_mode)` before all SDL_LogInfo calls
   - Remove boot_object_analyzer.cpp inclusion