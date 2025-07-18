#!/usr/bin/env python3

# Complete splash sequence analysis - Object Type 17 evolution

print('FM2K Boot Sequence Object Analysis: Type 17 Evolution')
print('=' * 70)

# Key changing offsets identified
state_offset = 382  # State/mode field
counter_offset = 386  # Timer/counter field

# Data from all three splash screens
screens = {
    'Splash 1': {'state': 0x01, 'counter': 0xEE},
    'Splash 2': {'state': 0x03, 'counter': 0x9F},  
    'Splash 3': {'state': 0x05, 'counter': 0x6B}
}

print('Object Type 17 (Boot/Splash Object) State Transitions:')
print()

print('Screen      State Field    Counter Field    State Change    Counter Change')
print('-------     -----------    -------------    ------------    --------------')

prev_state = None
prev_counter = None

for screen_name, data in screens.items():
    state = data['state']
    counter = data['counter']
    
    state_change = f"+{state - prev_state}" if prev_state is not None else "initial"
    counter_change = f"{counter - prev_counter:+d}" if prev_counter is not None else "initial"
    
    print(f'{screen_name:<12} 0x{state:02X} ({state:3d})      0x{counter:02X} ({counter:3d})        {state_change:<12} {counter_change}')
    
    prev_state = state
    prev_counter = counter

print()
print('Pattern Analysis:')
print('- Object Type: 0x11 (17) - CONSISTENT across all screens')
print('- Object ID: 0x7F (127) - CONSISTENT across all screens')  
print('- State Field: Increments by +2 each screen (1 → 3 → 5)')
print('- Counter Field: Decreases each screen (238 → 159 → 107)')
print('- Static Data: 380/382 bytes unchanged throughout sequence')

print()
print('Giuroll-Style Rollback Strategy:')
print('For Object Type 17 (Boot/UI objects):')
print('  - Save only 2 bytes: offsets 382-383 (state) + 386-387 (counter)')
print('  - Skip remaining 378 bytes (static data)')
print('  - Memory savings: 98.4% reduction (8 bytes vs 382 bytes)')

print()
print('Rollback Implementation Insights:')
print('1. OBJECT IDENTIFICATION: Type field (offset 0) determines save strategy')
print('2. DYNAMIC FIELDS ONLY: Track which offsets actually change per type')
print('3. PATTERN-BASED SAVING: Different object types have different dynamic fields')
print('4. MEMORY EFFICIENCY: Massive savings possible (380+ bytes → 8 bytes)')

print()
print('Next Phase - Character Objects:')
print('- Boot sequence uses Type 17 (UI/system objects)')
print('- Character select/battle will have Type 4 (character objects)')
print('- Need to map dynamic fields for each object type')
print('- Build type-specific save/restore functions')

print()
print('Prototype Giuroll-Style Save Structure:')
print('```')
print('struct FM2K_ObjectSave {')
print('    uint16_t slot_index;')
print('    uint8_t object_type;')
print('    uint8_t field_count;')
print('    struct {')
print('        uint16_t offset;')
print('        uint16_t size;')
print('        uint8_t data[];')
print('    } fields[];')
print('};')
print('```')

print()
print('For our Type 17 object:')
print('- slot_index: 1')
print('- object_type: 17')
print('- field_count: 2')
print('- field[0]: offset=382, size=1, data=[state_value]')
print('- field[1]: offset=386, size=1, data=[counter_value]')
print('- Total size: 12 bytes (vs 382 bytes full object)')