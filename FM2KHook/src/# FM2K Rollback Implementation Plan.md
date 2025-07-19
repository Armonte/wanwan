# FM2K Simplified Lockstep Implementation

## Overview
This document outlines the simplified approach to FM2K rollback netcode, focusing on lockstep synchronization for character select and menus, with rollback only enabled during battle.

## Key Changes Made

### 1. Simplified State Machine Logic
- **Character Select**: Uses `LOCKSTEP` synchronization
- **Battle**: Uses `ROLLBACK` netcode only after stabilization
- **Menus**: Uses `LOCKSTEP` for consistent navigation

### 2. Character Select Synchronization
- Added `CharSelectSync` class with confirmation handling
- Tracks both players' character selections and confirmations
- Uses GekkoNet's input system for confirmation signals
- Prevents battle transition until both players confirm

### 3. Simplified Hook Logic
- **SaveEvent**: Only does full saves during stable battle
- **LoadEvent**: Only does full loads during stable battle
- **Character Select**: Uses minimal saves (8 bytes) for lockstep
- **Menus**: Uses minimal saves for consistent state

### 4. Rollback Activation
- Only activates during `IN_BATTLE` phase after 60+ frames
- Disables during all transition periods
- Uses state machine to determine sync strategy

## Implementation Details

### Character Confirmation Flow
1. Player selects character (cursor movement tracked)
2. Player confirms selection (confirmation status tracked)
3. Local confirmation sent via GekkoNet input system
4. Both confirmations received before battle transition
5. State machine signals readiness for battle

### Lockstep vs Rollback
- **Lockstep**: Frame-perfect sync for menus and character select
- **Rollback**: Full state saving/loading only during battle
- **Transition**: Stabilization period prevents desyncs

### Memory Addresses Used
- Character cursor positions: `0x424E50-0x424E5C`
- Selected characters: `0x470020-0x470024`
- Confirmation status: `0x47019C-0x4701A0`
- Game mode: `0x470054`

## Benefits
1. **Simpler**: Less complex than full rollback everywhere
2. **Stable**: Lockstep prevents menu desyncs
3. **Focused**: Rollback only where needed (battle)
4. **Reliable**: Confirmation system ensures both players ready

## Testing Strategy
1. Test character select synchronization
2. Verify both players see same state
3. Test confirmation flow
4. Verify battle transition works
5. Test rollback during combat only
