#!/usr/bin/env python3

# Complete splash sequence analysis - Major transformation detected!

print('FM2K Boot Sequence - FINAL ANALYSIS: Object Transformation Detected!')
print('=' * 80)

# Previous splash data (Splash 3)
splash3_data = {
    'type': 0x11, 'id': 0x7F, 'field_08': 0x00, 'field_0C': 0x00,
    'state_382': 0x05, 'counter_386': 0x6B
}

# Final splash data (Controls screen) - MAJOR CHANGE
controls_data = {
    'type': 0x5E, 'id': 0x7F, 'field_08': 0x4703, 'field_0C': 0x00,
    # New negative values appear
    'field_75': 0xFFFFFFD8,  # -40 as signed int32
    'field_79': 0xFFFFFFD8,  # -40 as signed int32  
    'field_83': 0xFFFFFFE2,  # -30 as signed int32
    'field_91': 0x05,
    'field_382': 0x07
}

print('CRITICAL DISCOVERY: Object Type Changed!')
print()
print('Previous State (Splash 1-3):')
print(f'  Type: 0x{splash3_data["type"]:02X} ({splash3_data["type"]}) - UI/Splash object')
print(f'  ID: 0x{splash3_data["id"]:02X} ({splash3_data["id"]})')
print(f'  State: 0x{splash3_data["state_382"]:02X} Counter: 0x{splash3_data["counter_386"]:02X}')

print()
print('Current State (Controls screen):')
print(f'  Type: 0x{controls_data["type"]:02X} ({controls_data["type"]}) - NEW OBJECT TYPE!')
print(f'  ID: 0x{controls_data["id"]:02X} ({controls_data["id"]}) - Same ID maintained')

print()
print('New Object Type 0x5E (94) Field Analysis:')
print('  Offset   0: 0x5E (94) - Object type (CHANGED from 17)')
print('  Offset   4: 0x4703 (18179) - Possible pointer/reference')
print('  Offset  11: 0x7F (127) - ID maintained from previous object')
print('  Offset  75: 0xFFFFFFD8 (-40) - Signed coordinate/position?')
print('  Offset  79: 0xFFFFFFD8 (-40) - Signed coordinate/position?')
print('  Offset  83: 0xFFFFFFE2 (-30) - Signed coordinate/position?')
print('  Offset  91: 0x05 (5) - Some counter/state')
print('  Offset 382: 0x07 (7) - State field evolved from splash sequence')

print()
print('Object Lifecycle Discovery:')
print('1. TRANSFORMATION: Object slot 1 reused but completely changed type')
print('2. ID PERSISTENCE: Same ID (127) maintained across transformation')
print('3. TYPE EVOLUTION: 17 (splash/UI) → 94 (controls/interactive)')
print('4. NEW FIELDS: Position coordinates, pointers, different state layout')

print()
print('Rollback Implications - MAJOR:')
print('✓ Objects can transform types in same slot')
print('✓ Type field (offset 0) is CRITICAL for rollback strategy')
print('✓ Each type has completely different dynamic field layout')
print('✓ Cannot assume static object structure during game session')

print()
print('Giuroll-Style Strategy Refined:')
print('```cpp')
print('enum ObjectSaveStrategy {')
print('    TYPE_17_SPLASH,  // Save offsets: 382, 386')
print('    TYPE_94_CONTROLS, // Save offsets: 0, 4, 75, 79, 83, 91, 382')
print('    TYPE_4_CHARACTER, // TBD - character select/battle analysis needed')
print('    // ... more types discovered during gameplay')
print('};')
print('')
print('struct TypeSpecificSave {')
print('    uint8_t object_type;')
print('    uint8_t field_count;')
print('    uint16_t offsets[MAX_DYNAMIC_FIELDS];')
print('    uint8_t field_sizes[MAX_DYNAMIC_FIELDS];')
print('};')
print('```')

print()
print('Next Research Phase:')
print('- Move to character select screen')
print('- Analyze Type 4 character objects')
print('- Map character-specific dynamic fields (position, health, animation, etc)')
print('- Build complete object type → field mapping system')

print()
print('Key Insight:')
print('FM2K objects are more dynamic than expected - they can completely')
print('transform types within the same slot. This means our rollback system')
print('must be type-aware and handle object lifecycle changes correctly.')

print()
print('Memory Efficiency Still Massive:')
type_94_fields = 7  # offsets: 0,4,75,79,83,91,382 (assuming 4 bytes each)
estimated_save_size = type_94_fields * 4 + 4  # fields + header
print(f'Type 94 estimated save: {estimated_save_size} bytes vs 382 bytes full object')
print(f'Reduction: {100 * (1 - estimated_save_size/382):.1f}%')