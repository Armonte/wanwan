# FM2K MCP Research Feedback Loop

## Quick Reference for Systematic Fighting Game Analysis

**Project**: FM2K - Fighter Maker 2nd  
**Status**: Reverse engineering closed-source fighting game engine  
**Primary Target**: 
- **FM2K game executables** (e.g., WonderfulWorld_ver_0946.exe) - Main game executables with all core systems

## 📋 **Analysis Strategy**

### Why Start with FM2K executables?
1. **Complete System** - Contains all game logic in one binary
2. **High Symbol Density** - Many functions already identified through analysis
3. **Comprehensive Coverage** - Character system, rendering, input, audio, UI all in one place
4. **Foundation First** - Understanding core systems makes rollback netcode and tooling easier
5. **Rollback Ready** - Fixed 100 FPS (10ms per frame), no frame skip, perfect for rollback implementation

### What to Look For in FM2K executables:
- **Character System**: Character data structures, animation, hitboxes, collision
- **Input System**: 11-bit input mask, 1024-frame input history, button mapping, edge detection
- **Rendering System**: Sprite rendering, camera, effects
- **UI System**: Menu navigation, character select, options, HUD
- **Audio System**: Sound effects, BGM, voice playback
- **Network System**: Multiplayer, replay system, state synchronization
- **Timing System**: Frame counter (100 FPS), frame limiter, RNG state (CRITICAL for rollback)

---

## 🎯 **CRITICAL: Why Renaming is Essential**

**Renaming is THE most important part of reverse engineering - it transforms chaos into clarity!**

### Without Renaming (Nightmare Mode):
```c
// Completely unreadable - what does this do?
int __fastcall sub_45E500(int a1) {
  int v3 = *(int *)(a1 + 88);
  if (v3 == -1) {
    *(int *)(a1 + 92) = dword_8A53A0[*(int *)(a1 + 84)];
    dword_627C44[1532 * a2] = *(int *)(a1 + 156);
  }
  return sub_401F10(a1, byte_627BFC);
}
```

### With Proper Renaming (Crystal Clear):
```c
// Instantly understandable - increments frame counter for rollback!
int __fastcall IncrementFrameCounter() {
  if (g_rollback_active) {
    g_rollback_frame_counter++;
    return g_rollback_frame_counter;
  }
  g_frame_counter++;
  return g_frame_counter;
}
```

### 🚀 **Renaming Benefits:**
1. **Instant Code Comprehension** - No mental translation needed
2. **Pattern Recognition** - Similar functions become obvious  
3. **Bug Prevention** - Descriptive names prevent mistakes
4. **Knowledge Preservation** - Future you (and others) will thank you
5. **Research Acceleration** - Each rename makes the next discovery easier

### ⚡ **Renaming Multiplier Effect:**
- 1 renamed function → Makes 5 related functions clearer
- 1 renamed global → Makes 20 functions more readable  
- 1 renamed struct field → Unlocks understanding of entire data system

**RULE: Rename IMMEDIATELY after discovering meaning - don't postpone this step!**

### 🔄 **Research Cycle Workflow**

#### Phase 1: Function Analysis
```python
# 1. Get current function being analyzed
mcp_ida-pro-mcp_get_current_function()

# 2. Decompile for structure understanding
mcp_ida-pro-mcp_decompile_function(address="0x4146D0")  # FRAME_HOOK_ADDR

# 3. Get cross-references to understand usage
mcp_ida-pro-mcp_get_xrefs_to(address="0x4146D0")  # FRAME_HOOK_ADDR
# Note: get_xrefs_to shows all references (both callers and callees)
```

#### Phase 2: Variable Discovery
```python
# 1. List globals with common unknown prefixes
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=50, filter="dword_")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=50, filter="byte_")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=50, filter="unk_")

# 2. Get stack frame variables for key functions
mcp_ida-pro-mcp_get_stack_frame_variables(function_address="0x404CD0")  # UPDATE_GAME_STATE_ADDR
```

#### Phase 3: Data Structure Analysis
```python
# 1. Analyze structures referenced by functions
mcp_ida-pro-mcp_get_defined_structures()

# 2. Get struct usage at specific addresses
mcp_ida-pro-mcp_get_struct_at_address(address="0x470100", struct_name="PlayerState")  # P1_INPUT_ADDR
```

#### Phase 4: Renaming and Documentation (MOST CRITICAL PHASE!)
```python
# 1. RENAME IMMEDIATELY - This is the most important step!
mcp_ida-pro-mcp_rename_function(function_address="0x4146D0", new_name="ProcessFrame")  # FRAME_HOOK_ADDR
mcp_ida-pro-mcp_rename_global_variable(old_name="dword_470000", new_name="g_input_buffer_index")  # INPUT_BUFFER_INDEX_ADDR
mcp_ida-pro-mcp_rename_local_variable(function_address="0x404CD0", old_name="a1", new_name="game_state_ptr")  # UPDATE_GAME_STATE_ADDR

# 2. CREATE STRUCT TYPES for data structures
mcp_ida-pro-mcp_declare_c_type(c_declaration="struct PlayerState { uint32_t input_current; uint32_t input_history[1024]; uint32_t stage_x; uint32_t stage_y; uint32_t hp; uint32_t max_hp; ... };")
mcp_ida-pro-mcp_set_local_variable_type(function_address="0x404CD0", variable_name="game_state_ptr", new_type="PlayerState*")

# 3. Add comments AFTER renaming (comments are secondary to renaming!)
mcp_ida-pro-mcp_set_comment(address="0x4146D0", comment="Frame processing hook - called every frame (100 FPS = 10ms per frame)")
```

---

## 🎯 **Priority Target Functions**

### **Phase 1: Core Game Systems (START HERE)**

#### High Priority (Game Loop & Timing System - CRITICAL FOR ROLLBACK)
- Frame processing hook (`FRAME_HOOK_ADDR` @ 0x4146D0) - Main frame processing function
- Game state update (`UPDATE_GAME_STATE_ADDR` @ 0x404CD0) - Updates game state each frame
- Input buffer management (`INPUT_BUFFER_INDEX_ADDR` @ 0x470000) - Current input buffer index
- Random seed (`RANDOM_SEED_ADDR` @ 0x41FB1C) - RNG state (CRITICAL for rollback determinism)
- Frame timing system - 100 FPS (10ms per frame) timing management

#### Medium Priority (Combat & Physics)
- Character collision detection - Hitbox/hurtbox collision system
- Character animation - Animation state machine and frame processing
- Character attack sequence - Move execution and combo system
- Input reading and processing - 11-bit input mask reading
- Input history management - 1024-frame input history buffer
- Player state management - HP, position, timers, flags

#### Research Targets (Key Systems)
```python
# Start by listing all imports - reveals libraries used
mcp_ida-pro-mcp_list_imports(offset=0, count=100)

# List all functions to see what's available
mcp_ida-pro-mcp_list_functions(offset=0, count=50)

# Look for timing/frame related strings
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="frame")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="timer")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="input")
```

### **Phase 2: Rendering & UI Systems**

#### High Priority (Visual Systems)
- Sprite rendering (`RenderSprite` @ 0x40D420) - Needs deeper analysis
- Animation processing (`ProcessAnimationFrame` @ 0x42D860) - Needs deeper analysis
- Render system (`ProcessRenderSystem` @ 0x408c10) ✅ RENAMED
- Buffer swapping (`SwapRenderBuffers` @ 0x47bf70) ✅ RENAMED

#### Research Strategy
```python
# Find rendering-related strings
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="render")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="sprite")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="camera")
```

### 🚨 **COMPLEX FUNCTIONS - PRIORITY RENAMING TARGETS**

These functions have high unnamed variable density and need systematic analysis:

#### **Identification Strategy**:
```python
# Look for functions with high unnamed variable counts
# Prioritize by:
# 1. Function size (larger = more complex)
# 2. Number of magic offsets (character_data_ptr + XXX, game_state_ptr + XXX)
# 3. Number of unnamed variables (v1, v2, v3, etc.)
# 4. Cross-reference count (more references = more important)
# 5. String references (debug strings, error messages reveal purpose)

# Example analysis workflow:
mcp_ida-pro-mcp_decompile_function(address="0x404CD0")  # UPDATE_GAME_STATE_ADDR
mcp_ida-pro-mcp_get_xrefs_to(address="0x404CD0")
# Note: get_xrefs_to shows all references (both callers and callees)
```

#### **Critical Renaming Rule**:
**xref globals and rename them too** <-- VERY IMPORTANT  
When you find a function using unnamed globals (dword_XXXXX, byte_XXXXX), trace their usage and rename them immediately!

#### **Complex Function Analysis Template**:
For each complex function discovered:
1. Decompile and analyze parameter usage
2. Identify all global variable accesses → rename globals
3. Map local variable purposes → rename locals
4. Identify function purpose → rename function
5. Document in research notes

---

## 🏗️ **Structure Creation & Manipulation Workflow**

### Overview: Why Structures are Critical
Creating proper C structures in IDA transforms pointer arithmetic into meaningful field names!

**Before Structure:**
```c
int value = *(int *)(character_data + 12);  // What is offset +12?
```

**After Structure:**
```c
int value = character_data->current_hp;  // Crystal clear!
```

### Complete Structure Workflow

#### Step 1: Discover Structure Usage Pattern
```python
# Find functions that access the same memory region with different offsets
mcp_ida-pro-mcp_decompile_function(address="0x4306d0")
# Look for patterns like:
#   *(int *)(a1 + 0)   → field at offset 0
#   *(int *)(a1 + 4)   → field at offset 4
#   *(short *)(a1 + 8) → field at offset 8
```

#### Step 2: Check Existing Structures
```python
# List all structures already defined in IDA
mcp_ida-pro-mcp_get_defined_structures()

# Search for relevant structures
mcp_ida-pro-mcp_search_structures(filter="Character")
mcp_ida-pro-mcp_search_structures(filter="Hitbox")
mcp_ida-pro-mcp_search_structures(filter="Input")
```

#### Step 3: Create New Structure (or Update Existing)
```python
# Example 1: Create a character data structure (260 bytes)
mcp_ida-pro-mcp_declare_c_type(c_declaration="""
struct CharacterData {
    uint32_t position_x;              // +0x00
    uint32_t position_y;              // +0x04
    uint32_t velocity_x;              // +0x08
    uint32_t velocity_y;              // +0x0C
    uint32_t current_hp;              // +0x10
    uint32_t max_hp;                  // +0x14
    uint32_t current_frame;           // +0x18
    uint32_t animation_state;         // +0x1C
    uint16_t facing_direction;        // +0x20 (0=left, 1=right)
    uint16_t state_flags;             // +0x22
    uint32_t hitbox_count;            // +0x24
    void* hitbox_array_ptr;            // +0x28
    // ... remaining ~232 bytes
};
""")

# Example 2: Create hitbox structure
mcp_ida-pro-mcp_declare_c_type(c_declaration="""
struct HitboxData {
    int16_t x_offset;                 // +0x00
    int16_t y_offset;                  // +0x02
    uint16_t width;                    // +0x04
    uint16_t height;                   // +0x06
    uint8_t hitbox_type;               // +0x08 (0=hurtbox, 1=hitbox, 2=throwbox)
    uint8_t damage;                    // +0x09
    uint16_t hitstun;                  // +0x0A
    uint16_t blockstun;                // +0x0C
    // ... more fields
};
""")

# Example 3: Create input state structure
mcp_ida-pro-mcp_declare_c_type(c_declaration="""
struct InputState {
    uint32_t current_inputs;           // +0x00 - Bitfield of current button presses
    uint32_t previous_inputs;          // +0x04 - Bitfield of previous frame inputs
    uint32_t edge_detection;           // +0x08 - Buttons pressed this frame (edge)
    int16_t analog_x;                  // +0x0C - Analog stick X
    int16_t analog_y;                  // +0x0E - Analog stick Y
};
""")
```

#### Step 4: Apply Structure Types to Variables
```python
# Apply structure type to local variables
mcp_ida-pro-mcp_set_local_variable_type(
    function_address="0x4306d0",
    variable_name="a1",
    new_type="CharacterData*"
)

# Apply structure type to global variables
mcp_ida-pro-mcp_set_global_variable_type(
    variable_name="g_player1_character_data",
    new_type="CharacterData*"
)
```

### 🎯 Complete Structure Discovery Example

**Scenario**: You found a function that processes character hitboxes. Let's reverse engineer the hitbox structure!

```python
# Step 1: Decompile the function
mcp_ida-pro-mcp_decompile_function(address="0x418370")

# Output shows:
# int __fastcall ProcessCharacterCollision(void *hitbox_ptr) {
#   int x = *(int16_t *)(hitbox_ptr + 0);
#   int y = *(int16_t *)(hitbox_ptr + 2);
#   int width = *(uint16_t *)(hitbox_ptr + 4);
#   int height = *(uint16_t *)(hitbox_ptr + 6);
#   int type = *(uint8_t *)(hitbox_ptr + 8);
#   ...
# }

# Step 2: Check if structure already exists
mcp_ida-pro-mcp_search_structures(filter="Hitbox")
# (let's say it doesn't exist)

# Step 3: Create the structure based on observed offsets
mcp_ida-pro-mcp_declare_c_type(c_declaration="""
struct HitboxData {
    int16_t x_offset;      // +0x00
    int16_t y_offset;       // +0x02
    uint16_t width;         // +0x04
    uint16_t height;        // +0x06
    uint8_t hitbox_type;    // +0x08
    // ... more to discover
};
""")

# Step 4: Apply the structure type to the parameter
mcp_ida-pro-mcp_set_local_variable_type(
    function_address="0x418370",
    variable_name="hitbox_ptr",
    new_type="HitboxData*"
)

# Step 5: Rename the function now that we understand it
mcp_ida-pro-mcp_rename_function(
    function_address="0x418370",
    new_name="ProcessCharacterCollision"
)
```

---

## 🔍 **Data Structure Research Pattern**

### Step 1: Identify Structure Access
```python
# Look for functions accessing character/game state structures
# Common patterns in fighting games:
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=100, filter="character")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=100, filter="hitbox")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=100, filter="input")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=100, filter="frame")
```

### Step 2: Analyze Access Patterns
```python
# Decompile functions that access the structure
mcp_ida-pro-mcp_decompile_function(address="found_function_address")

# Look for common fighting game structures:
# - Character data (HP, position, state, animation)
# - Hitbox arrays (collision boxes for attacks)
# - Input state (button presses, edge detection)
# - Frame timing data
```

### Step 3: Map Field Meanings
```python
# Look for field offset patterns in fighting game context:
# - HP values (uint32_t, typically 0-10000 range)
# - Position data (int16_t or int32_t, screen coordinates)
# - Animation frame numbers (uint32_t, incrementing counters)
# - State flags (uint8_t bitfields, invulnerable, blocking, etc.)
# - Hitbox data (arrays of collision rectangles)
# - Input bitfields (uint32_t, one bit per button)

# Document field meanings in research guide
```

### Common FM2K Structures to Look For:
```c
// Player State Structure (from FM2K_Integration.h)
struct PlayerState {
    uint32_t input_current;            // Current input state (11-bit mask)
    uint32_t input_history[1024];      // Input history buffer (1024 frames)
    uint32_t stage_x, stage_y;         // Position on stage
    uint32_t hp, max_hp;               // Health points
    uint32_t meter, max_meter;         // Super meter
    uint32_t combo_counter;            // Combo count
    uint32_t hitstun_timer;            // Hitstun frames remaining
    uint32_t blockstun_timer;          // Blockstun frames remaining
    uint32_t anim_timer;               // Animation timer
    uint32_t move_id;                  // Current move ID
    uint32_t state_flags;              // Bitfield: invulnerable, blocking, etc.
};

// Hitbox Data Structure (from FM2K_Integration.h)
struct HitBox {
    int32_t x, y, w, h;                // Hitbox rectangle
    uint32_t type;                     // Hitbox type
    uint32_t damage;                   // Damage value
    uint32_t flags;                    // Hitbox flags
};

// Input Structure (11-bit input mask)
struct Input {
    union {
        struct {
            uint16_t left     : 1;
            uint16_t right    : 1;
            uint16_t up       : 1;
            uint16_t down     : 1;
            uint16_t button1  : 1;
            uint16_t button2  : 1;
            uint16_t button3  : 1;
            uint16_t button4  : 1;
            uint16_t button5  : 1;
            uint16_t button6  : 1;
            uint16_t button7  : 1;
            uint16_t reserved : 5;
        } bits;
        uint16_t value;
    };
};
```

---

## 📊 **Validation Checklist**

### ✅ **Confirmed Discoveries** (Update as you progress)
- [x] Player state structure mapped (from FM2K_Integration.h)
- [x] Input system identified (11-bit mask, 1024-frame history)
- [x] Frame processing hook identified (FRAME_HOOK_ADDR @ 0x4146D0)
- [x] Game state update identified (UPDATE_GAME_STATE_ADDR @ 0x404CD0)
- [x] RNG system identified (RANDOM_SEED_ADDR @ 0x41FB1C)
- [x] Memory addresses mapped (player HP, position, input buffers)
- [ ] Complete object pool structure mapping
- [ ] Hitbox collision system fully analyzed
- [ ] Complete input reading and edge detection system
- [ ] Rendering pipeline mapped
- [ ] UI system structure understood

### ⚠️ **Priority Research Targets**
- **Timing System**: Frame counter (100 FPS), frame limiter, RNG state (CRITICAL for rollback)
- **Character System**: Complete object pool structure mapping, animation states
- **Combat System**: Hitbox detection, damage calculation, frame data
- **Input System**: Complete 11-bit button mapping, edge detection, 1024-frame input history
- **Rendering System**: Sprite rendering, camera, interpolation
- **UI System**: Menu navigation, character select, HUD display

### 🔍 **Key Questions to Answer**
1. What is the complete object pool structure layout? (OBJECT_POOL_ADDR @ 0x4701E0)
2. How does hitbox collision detection work?
3. What is the complete input reading and processing flow? (11-bit mask, 1024-frame history)
4. How are sprites and animations managed?
5. What is the frame timing system? (100 FPS = 10ms per frame)
6. How does the UI menu system work?
7. Where is the RNG seed stored? (RANDOM_SEED_ADDR @ 0x41FB1C - CRITICAL for rollback determinism)

---

## 🚀 **Quick Start Commands**

### Begin New Research Session
```python
# 1. Check IDA connection
mcp_ida-pro-mcp_check_connection()

# 2. Get current analysis context
mcp_ida-pro-mcp_get_current_address()
mcp_ida-pro-mcp_get_metadata()

# 3. List all imports - reveals libraries used
mcp_ida-pro-mcp_list_imports(offset=0, count=100)

# 4. List all functions
mcp_ida-pro-mcp_list_functions(offset=0, count=50)

# 5. Check for entry points
mcp_ida-pro-mcp_get_entry_points()

# 6. Analyze known hook points
mcp_ida-pro-mcp_decompile_function(address="0x4146D0")  # FRAME_HOOK_ADDR
mcp_ida-pro-mcp_decompile_function(address="0x404CD0")  # UPDATE_GAME_STATE_ADDR
```

### Identify Timing System (CRITICAL FOR ROLLBACK)
```python
# 1. Look for timing/frame related strings
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="frame")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="timer")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="time")

# 2. Look for input-related strings (11-bit input mask, 1024-frame history)
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="input")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="button")

# 3. Analyze frame processing hook
mcp_ida-pro-mcp_decompile_function(address="0x4146D0")  # FRAME_HOOK_ADDR
mcp_ida-pro-mcp_get_xrefs_to(address="0x4146D0")
```

### Find Combat System Functions
```python
# 1. Look for combat-related strings
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="damage")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="hit")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="collision")

# 2. Look for input-related strings
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="input")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="button")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="key")
```

### Analyze Function Relationships
```python
# 1. Find a key function (e.g., frame processing hook)
mcp_ida-pro-mcp_decompile_function(address="0x4146D0")  # FRAME_HOOK_ADDR

# 2. Find all references to this function (shows both callers and callees)
mcp_ida-pro-mcp_get_xrefs_to(address="0x4146D0")

# 3. Analyze what the function calls by decompiling it
# The decompiled code will show all function calls within

# 5. Analyze game state update function
mcp_ida-pro-mcp_decompile_function(address="0x404CD0")  # UPDATE_GAME_STATE_ADDR
mcp_ida-pro-mcp_get_xrefs_to(address="0x404CD0")
```

---

## 📝 **Documentation Update Pattern**

### After Each Discovery (CRITICAL ORDER - FOLLOW EXACTLY!)
1. 🔥 **RENAME IMMEDIATELY** - Don't postpone! Use MCP tools to rename functions, globals, and locals
2. 🏗️ **CREATE STRUCT TYPES** - Declare C types for discovered data structures  
3. 🎯 **SET VARIABLE TYPES** - Apply struct types to function parameters and locals
4. 🔥 **RENAME LOCAL VARIABLES** - Transform v1,v2,v3 into meaningful names IMMEDIATELY!
5. 📝 **Add comments** to complex logic sections while understanding is fresh (AFTER renaming!)
6. 📋 **Update Field Research Guide** with new findings and renamed symbols
7. ✅ **Validate findings** through multiple function analysis (now much easier with renamed code!)
8. 📋 **Update TODO list** with progress and next steps

### 🎯 **COMPLEX FUNCTION RENAMING WORKFLOW**

For functions with 10+ unnamed variables, use this systematic approach:

#### **Step 1: Identify Variable Purposes**
```python
# Decompile the function and analyze usage patterns
mcp_ida-pro-mcp_decompile_function(address="0x4306d0")

# Look for patterns in fighting game code:
# - Variables accessing character_data + offset → character field access
# - Variables accessing hitbox_data + offset → hitbox field access
# - Variables in conditionals → state/flag variables (invulnerable, blocking, etc.)
# - Variables in loops → counters/iterators (frame loops, hitbox loops)
# - Variables passed to functions → parameters (sprite_id, damage, frame_count)
# - Variables from function returns → results (collision_result, damage_dealt)
# - Variables compared to constants → enum values (hitbox_type, animation_state)
```

#### **Step 2: Rename Systematically (DO NOT SKIP!)**
```python
# Example: Renaming a character processing function
mcp_ida-pro-mcp_rename_local_variable(function_address="0x4306d0", old_name="v3", new_name="current_hp")
mcp_ida-pro-mcp_rename_local_variable(function_address="0x4306d0", old_name="v4", new_name="damage_value")
mcp_ida-pro-mcp_rename_local_variable(function_address="0x4306d0", old_name="v5", new_name="frame_number")
mcp_ida-pro-mcp_rename_local_variable(function_address="0x4306d0", old_name="a1", new_name="character_data_ptr")
mcp_ida-pro-mcp_rename_local_variable(function_address="0x4306d0", old_name="a2", new_name="hitbox_array_ptr")
# Continue for ALL variables!
```

### 🚨 **Renaming Priority Rules (MANDATORY WORKFLOW):**
- **Functions**: Rename the moment you understand what they do
- **Globals**: Rename when you see their usage pattern (`dword_66ECCC` → `g_frame_counter`, `dword_4AB758` → `g_controller1_enabled`, etc.)
- **Locals**: Rename parameters and key variables in important functions (`a1` → `character_data_ptr`, `a2` → `hitbox_ptr`)
- **Structs**: Update field names as soon as you decode their meaning AND create C struct types
- **Types**: Apply struct types to variables to get automatic field name resolution

### 📋 **Common FM2K Global Variable Naming Patterns:**
```
Input System:
- `g_input_buffer_index` @ 0x470000 (INPUT_BUFFER_INDEX_ADDR)
- `g_player1_input_current` @ 0x470100 (P1_INPUT_ADDR) - 11-bit input mask
- `g_player1_input_history` @ 0x470200 (P1_INPUT_HISTORY_ADDR) - 1024-frame history
- `g_player2_input_current` @ 0x470300 (P2_INPUT_ADDR) - 11-bit input mask
- `g_player2_input_history` @ 0x470400 (P2_INPUT_HISTORY_ADDR) - 1024-frame history

Player State:
- `g_player1_stage_x` @ 0x470104 (P1_STAGE_X_ADDR)
- `g_player1_stage_y` @ 0x470108 (P1_STAGE_Y_ADDR)
- `g_player1_hp` @ 0x47010C (P1_HP_ADDR)
- `g_player1_max_hp` @ 0x470110 (P1_MAX_HP_ADDR)
- `g_player2_hp` @ 0x47030C (P2_HP_ADDR)
- `g_player2_max_hp` @ 0x470310 (P2_MAX_HP_ADDR)

Frame/Timing:
- `g_round_timer` @ 0x470060 (ROUND_TIMER_ADDR)
- `g_game_timer` @ 0x470064 (GAME_TIMER_ADDR)
- `g_random_seed` @ 0x41FB1C (RANDOM_SEED_ADDR) - CRITICAL for rollback!

Game State:
- `g_object_pool` @ 0x4701E0 (OBJECT_POOL_ADDR) - Main game object pool

Effect System:
- `g_effect_active_flags` @ 0x40CC30 (EFFECT_ACTIVE_FLAGS) - Bitfield of active effects
- `g_effect_timers` @ 0x40CC34 (EFFECT_TIMERS_BASE) - Array of 8 effect timers
- `g_effect_colors` @ 0x40CC54 (EFFECT_COLORS_BASE) - Array of 8 RGB color sets
- `g_effect_targets` @ 0x40CCD4 (EFFECT_TARGETS_BASE) - Array of 8 target IDs

Hook Points:
- `FRAME_HOOK_ADDR` @ 0x4146D0 - Main frame processing function
- `UPDATE_GAME_STATE_ADDR` @ 0x404CD0 - Game state update function
```

### 🏗️ **STRUCT TYPE CREATION IS CRITICAL:**
```python
# Example: After discovering player state fields, CREATE the struct type:
mcp_ida-pro-mcp_declare_c_type(c_declaration="""
struct PlayerState {
    uint32_t input_current;            // +0x00 - 11-bit input mask
    uint32_t input_history[1024];       // +0x04 - 1024-frame history
    uint32_t stage_x;                   // +0x1004 - X position on stage
    uint32_t stage_y;                   // +0x1008 - Y position on stage
    uint32_t hp;                        // +0x100C - Current HP
    uint32_t max_hp;                    // +0x1010 - Maximum HP
    // ... more fields
};
""")

# Then apply the type to function parameters:
mcp_ida-pro-mcp_set_local_variable_type(
    function_address="0x404CD0",  # UPDATE_GAME_STATE_ADDR
    variable_name="player_state_ptr", 
    new_type="PlayerState*"
)
```

**WHY THIS ORDER MATTERS**: Each rename makes the next analysis step exponentially easier!

### Research Notes Format
```markdown
## Function: ProcessCharacterXXX (0x4306d0)
**Purpose**: [Discovered purpose - e.g., "Processes character logic update"]
**Parameters**: 
- a1 (renamed: character_data_ptr) - CharacterData*
- a2 (renamed: frame_number) - uint32_t

**Key Findings**: 
- Field +0x10: current_hp (character health points)
- Field +0x20: facing_direction (0=left, 1=right)
- Field +0x24: hitbox_count (number of active hitboxes)
- Enum values: FACING_LEFT=0, FACING_RIGHT=1

**Global Variables Used**:
- `dword_66ECCC` (renamed: `g_frame_counter`)
- `byte_627BFC` (needs analysis - accessed by input functions)

**Cross-References**: 
- Called by: ProcessGameLogic (0x48d080)
- Calls: ProcessCharacterCollision (0x418370), UpdateCharacterAnimation (0x42a2a0)

**Next Steps**: 
- Analyze ProcessCharacterCollision to understand collision algorithm
- Map complete CharacterData structure fields
```

---

## ⚡ **Quick Reference Commands**

### Essential Daily Research Commands
```python
# Connection & Context
mcp_ida-pro-mcp_check_connection()
mcp_ida-pro-mcp_get_metadata()
mcp_ida-pro-mcp_get_current_address()
mcp_ida-pro-mcp_get_current_function()

# Function Analysis
mcp_ida-pro-mcp_decompile_function(address="0x4146D0")  # FRAME_HOOK_ADDR
mcp_ida-pro-mcp_disassemble_function(start_address="0x4146D0")
mcp_ida-pro-mcp_get_function_by_name(name="ProcessFrame")
mcp_ida-pro-mcp_get_function_by_address(address="0x4146D0")
mcp_ida-pro-mcp_list_functions(offset=0, count=50)

# Cross-Reference Analysis
mcp_ida-pro-mcp_get_xrefs_to(address="0x4146D0")  # FRAME_HOOK_ADDR
# Note: get_xrefs_to shows all references (both callers and callees)
mcp_ida-pro-mcp_get_xrefs_to_field(struct_name="PlayerState", field_name="hp")

# String & Import Discovery
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=100, filter="frame")
mcp_ida-pro-mcp_list_strings(offset=0, count=100)
mcp_ida-pro-mcp_list_imports(offset=0, count=100)
mcp_ida-pro-mcp_get_entry_points()

# Global Variable Analysis
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=50, filter="dword_")
mcp_ida-pro-mcp_list_globals(offset=0, count=50)
mcp_ida-pro-mcp_get_global_variable_value_by_name(variable_name="g_input_buffer_index")
mcp_ida-pro-mcp_get_global_variable_value_at_address(address="0x470000")  # INPUT_BUFFER_INDEX_ADDR

# Renaming (CRITICAL!)
mcp_ida-pro-mcp_rename_function(function_address="0x4146D0", new_name="ProcessFrame")
mcp_ida-pro-mcp_rename_global_variable(old_name="dword_470000", new_name="g_input_buffer_index")
mcp_ida-pro-mcp_rename_local_variable(function_address="0x404CD0", old_name="a1", new_name="game_state_ptr")

# Type System
mcp_ida-pro-mcp_declare_c_type(c_declaration="struct PlayerState { uint32_t input_current; uint32_t input_history[1024]; ... };")
mcp_ida-pro-mcp_set_local_variable_type(function_address="0x404CD0", variable_name="a1", new_type="PlayerState*")
mcp_ida-pro-mcp_set_global_variable_type(variable_name="g_player1_state", new_type="PlayerState*")
mcp_ida-pro-mcp_set_function_prototype(function_address="0x404CD0", prototype="int __fastcall UpdateGameState(PlayerState* player_state, int frame)")

# Structure Analysis
mcp_ida-pro-mcp_get_defined_structures()
mcp_ida-pro-mcp_search_structures(filter="Player")
mcp_ida-pro-mcp_analyze_struct_detailed(name="PlayerState")
mcp_ida-pro-mcp_get_struct_at_address(address="0x470100", struct_name="PlayerState")
mcp_ida-pro-mcp_get_struct_info_simple(name="PlayerState")

# Stack Frame Analysis
mcp_ida-pro-mcp_get_stack_frame_variables(function_address="0x404CD0")  # UPDATE_GAME_STATE_ADDR
mcp_ida-pro-mcp_rename_stack_frame_variable(function_address="0x404CD0", old_name="var_10", new_name="player_hp")
mcp_ida-pro-mcp_set_stack_frame_variable_type(function_address="0x404CD0", variable_name="player_hp", type_name="uint32_t")

# Comments & Documentation
mcp_ida-pro-mcp_set_comment(address="0x4146D0", comment="Frame processing hook - called every frame (100 FPS)")
```

---

## 🔎 **String Analysis Strategy - The Gold Mine!**

**Strings are your best friend in reverse engineering!** Error messages, debug output, and format strings reveal function purposes instantly.

### Critical String Search Patterns
```python
# 1. ERROR MESSAGES (reveal what went wrong = reveal function purpose!)
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="error")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="fail")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="invalid")

# 2. TIMING/FRAME STRINGS (CRITICAL FOR ROLLBACK!)
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="frame")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="timer")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="time")

# 3. CHARACTER/COMBAT STRINGS
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="character")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="hitbox")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="damage")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="hp")

# 4. INPUT STRINGS
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="input")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="button")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="key")

# 5. FORMAT STRINGS (reveal data being printed!)
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="%d")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="%x")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="%s")
```

---

## 🎯 **Common Fighting Game Code Patterns to Recognize**

### Pattern 1: Frame Processing (CRITICAL FOR ROLLBACK)
```c
// Typical frame processing pattern (100 FPS = 10ms per frame)
void ProcessFrame() {
    // Update input buffer index
    g_input_buffer_index = (g_input_buffer_index + 1) % 1024;
    
    // Read inputs and store in history
    uint16_t input = ReadInput();
    g_player1_input_history[g_input_buffer_index] = input;
    
    // Process game logic
    UpdateGameState();
    
    // Update RNG state (CRITICAL for rollback determinism!)
    g_random_seed = UpdateRNG(g_random_seed);
}

// Look for:
// - Input buffer index management
// - 1024-frame input history updates
// - RNG seed updates
// - Frame timing synchronization (10ms per frame)
```

### Pattern 2: Hitbox Collision Detection
```c
// Typical collision detection pattern
bool CheckHitboxCollision(HitboxData* hitbox1, HitboxData* hitbox2) {
    // AABB collision detection
    if (hitbox1->x < hitbox2->x + hitbox2->width &&
        hitbox1->x + hitbox1->width > hitbox2->x &&
        hitbox1->y < hitbox2->y + hitbox2->height &&
        hitbox1->y + hitbox1->height > hitbox2->y) {
        return true;
    }
    return false;
}

// Look for:
// - Rectangle intersection checks
// - Hitbox vs hurtbox comparisons
// - Damage application after collision
```

### Pattern 3: Input Reading and History Management
```c
// Typical input processing pattern (11-bit mask, 1024-frame history)
void ReadInputs() {
    // Read current input (11-bit mask)
    uint16_t current = ReadRawInput();  // 11 bits: left, right, up, down, button1-7
    
    // Store in history buffer
    g_player1_input_history[g_input_buffer_index] = current;
    
    // Edge detection (buttons pressed this frame)
    uint16_t previous = g_player1_input_history[(g_input_buffer_index - 1) % 1024];
    uint16_t edge = current & ~previous;  // Buttons pressed this frame
    
    // Process edge detection for special moves
    if (edge & BUTTON_PUNCH) {
        // Button just pressed
    }
}

// Look for:
// - 11-bit input mask operations
// - 1024-frame history buffer management
// - Input buffer index wrapping (mod 1024)
// - Edge detection (current & ~previous)
// - Input buffering for special moves
```

### Pattern 4: Animation State Machine
```c
// Typical state machine pattern
void UpdateCharacterState(CharacterData* char_data) {
    uint32_t current_state = char_data->animation_state;
    uint32_t frame = char_data->current_frame;
    
    // Check for state transitions
    if (frame >= state_data[current_state].end_frame) {
        char_data->animation_state = state_data[current_state].next_state;
        char_data->current_frame = 0;
    }
}

// Look for:
// - State ID comparisons
// - Frame counter checks
// - State transition tables
```

### Pattern 5: Damage Calculation
```c
// Typical damage application pattern
void ApplyDamage(CharacterData* target, int damage, int hit_type) {
    if (target->state_flags & FLAG_INVULNERABLE) {
        return;  // No damage if invulnerable
    }
    
    if (target->state_flags & FLAG_BLOCKING && hit_type != HIT_LOW) {
        damage /= 2;  // Blocked attacks do less damage
    }
    
    target->current_hp -= damage;
    if (target->current_hp < 0) {
        target->current_hp = 0;
    }
}

// Look for:
// - HP subtraction
// - State flag checks (invulnerable, blocking)
// - Damage type handling (high/mid/low)
```

---

## ⚠️ **Troubleshooting Common Issues**

### Issue 1: "Tool call failed" or connection errors
```python
# Always verify connection first
mcp_ida-pro-mcp_check_connection()

# Check IDA metadata
mcp_ida-pro-mcp_get_metadata()

# Make sure IDA has the correct file loaded (FM2K game executable, e.g., WonderfulWorld_ver_0946.exe)
# Make sure the MCP server plugin is running in IDA
```

### Issue 2: Can't find a global variable
```python
# Global might be using a different prefix
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=20, filter="dword_")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=20, filter="qword_")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=20, filter="byte_")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=20, filter="unk_")
```

### Issue 3: Renaming fails
```python
# Make sure you're using the exact current name
mcp_ida-pro-mcp_get_function_by_address(address="0x45E500")
# Use the name shown in the response

# For local variables, make sure function address is correct
mcp_ida-pro-mcp_get_stack_frame_variables(function_address="0x4306d0")
# Check exact variable names before renaming
```

### Issue 4: Structure not applying correctly
```python
# Check if structure exists
mcp_ida-pro-mcp_search_structures(filter="YourStructName")

# View structure definition
mcp_ida-pro-mcp_analyze_struct_detailed(name="YourStructName")

# Make sure you're using pointer type if needed
# "CharacterData*" not "CharacterData" for pointer parameters
```

### Issue 5: Address format confusion
```python
# Use hex format with 0x prefix
mcp_ida-pro-mcp_decompile_function(address="0x4146D0")  # ✅ Correct (FRAME_HOOK_ADDR)

# EXE addresses typically start at 0x400000 (32-bit) or 0x140000000 (64-bit)
# FM2K games are 32-bit, so addresses start around 0x400000
# Known FM2K addresses are in FM2K_Integration.h
```

---

## 📊 **Progress Tracking Template**

Keep a separate notes file to track your discoveries:

```markdown
# FM2K Analysis - Progress Log

## Session: [Date]

### Functions Discovered & Renamed
- [x] 0x4146D0 → ProcessFrame (FRAME_HOOK_ADDR)
  - Purpose: Main frame processing function (CRITICAL for rollback)
  - Called by: Main game loop
  - Updates: Input buffer index, game state

- [x] 0x404CD0 → UpdateGameState (UPDATE_GAME_STATE_ADDR)
  - Purpose: Updates game state each frame
  - Parameters: Game state pointer
  - Returns: success/failure
  - Updates: Player states, hitboxes, timers

- [ ] Object pool processing (NEEDS ANALYSIS)
  - Address: OBJECT_POOL_ADDR @ 0x4701E0
  - Has complex object management logic

### Structures Defined
- [x] PlayerState (from FM2K_Integration.h)
  - +0x00: input_current (uint32_t) - 11-bit input mask
  - +0x04: input_history[1024] (uint32_t) - 1024-frame history
  - +0x1004: stage_x (uint32_t)
  - +0x1008: stage_y (uint32_t)
  - +0x100C: hp (uint32_t)
  - +0x1010: max_hp (uint32_t)
  - ... (more fields to discover)

- [ ] HitBox structure (from FM2K_Integration.h)
  - +0x00: x (int32_t)
  - +0x04: y (int32_t)
  - +0x08: w (int32_t)
  - +0x0C: h (int32_t)
  - +0x10: type (uint32_t)
  - +0x14: damage (uint32_t)
  - +0x18: flags (uint32_t)

### Global Variables Mapped
- [x] `INPUT_BUFFER_INDEX_ADDR` @ 0x470000 → `g_input_buffer_index` (CRITICAL for rollback!)
- [x] `RANDOM_SEED_ADDR` @ 0x41FB1C → `g_random_seed` (CRITICAL for rollback determinism!)
- [x] `P1_INPUT_ADDR` @ 0x470100 → `g_player1_input_current` (11-bit mask)
- [x] `P1_INPUT_HISTORY_ADDR` @ 0x470200 → `g_player1_input_history` (1024 frames)
- [x] `P2_INPUT_ADDR` @ 0x470300 → `g_player2_input_current` (11-bit mask)
- [x] `P2_INPUT_HISTORY_ADDR` @ 0x470400 → `g_player2_input_history` (1024 frames)
- [x] `P1_HP_ADDR` @ 0x47010C → `g_player1_hp`
- [x] `P2_HP_ADDR` @ 0x47030C → `g_player2_hp`
- [x] `OBJECT_POOL_ADDR` @ 0x4701E0 → `g_object_pool`
- [ ] Effect system addresses (needs deeper analysis)

### Key Questions Answered
✅ Where is frame processing hook? → FRAME_HOOK_ADDR @ 0x4146D0
✅ Where is RNG seed stored? → RANDOM_SEED_ADDR @ 0x41FB1C (CRITICAL for rollback!)
✅ What is input system? → 11-bit mask, 1024-frame history
✅ What is player state layout? → PlayerState structure (from FM2K_Integration.h)
⚠️ How does hitbox collision work? → Still investigating
⚠️ What is object pool structure? → Still investigating
⚠️ How does effect system work? → Still investigating

### Next Steps
1. Complete object pool structure mapping (OBJECT_POOL_ADDR @ 0x4701E0)
2. Analyze hitbox collision detection algorithm
3. Map complete input reading and edge detection system
4. Understand effect system (EFFECT_ACTIVE_FLAGS, timers, colors, targets)
5. Map complete timing system for rollback hooks (100 FPS = 10ms per frame)
```

---

## 🎬 **START HERE: FM2K Analysis Workflow**

### Step 1: Initial Reconnaissance (5-10 minutes)
```python
# Connect to IDA and get metadata
mcp_ida-pro-mcp_check_connection()
mcp_ida-pro-mcp_get_metadata()

# List ALL imports - reveals libraries used (DxLib, DirectInput, DirectPlay, etc.)
mcp_ida-pro-mcp_list_imports(offset=0, count=0)  # count=0 means all

# List ALL functions - see what's available
mcp_ida-pro-mcp_list_functions(offset=0, count=0)  # count=0 means all

# Get entry points
mcp_ida-pro-mcp_get_entry_points()
```

### Step 2: Identify Core Systems (10-15 minutes)
```python
# Search for timing/frame related strings (CRITICAL FOR ROLLBACK!)
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="frame")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="timer")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="time")

# Search for character/combat strings
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="character")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="hitbox")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="damage")

# Search for input strings
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="input")
mcp_ida-pro-mcp_list_strings_filter(offset=0, count=0, filter="button")
```

### Step 3: Map Core Functions (30-60 minutes)
```python
# For each important function found:
# 1. Decompile it
mcp_ida-pro-mcp_decompile_function(address="0x4146D0")  # FRAME_HOOK_ADDR

# 2. Find all references (shows both callers and callees)
mcp_ida-pro-mcp_get_xrefs_to(address="0x4146D0")

# 3. Analyze what it calls by examining the decompiled code
# The decompiled function will show all function calls within

# 4. RENAME IMMEDIATELY when you understand it!
mcp_ida-pro-mcp_rename_function(function_address="0x4146D0", new_name="ProcessFrame")

# 5. Rename parameters
mcp_ida-pro-mcp_rename_local_variable(function_address="0x404CD0", old_name="a1", new_name="game_state_ptr")
```

### Step 4: Map Global State (20-30 minutes)
```python
# List globals by common prefixes
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=0, filter="dword_")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=0, filter="byte_")
mcp_ida-pro-mcp_list_globals_filter(offset=0, count=0, filter="qword_")

# For each interesting global, find where it's used
mcp_ida-pro-mcp_get_xrefs_to(address="0x470000")  # INPUT_BUFFER_INDEX_ADDR

# RENAME when you understand it
mcp_ida-pro-mcp_rename_global_variable(old_name="dword_470000", new_name="g_input_buffer_index")
```

### Step 5: Document Structures (ongoing)
```python
# As you discover data structures, create them
mcp_ida-pro-mcp_declare_c_type(c_declaration="""
struct PlayerState {
    uint32_t input_current;
    uint32_t input_history[1024];
    uint32_t stage_x;
    uint32_t stage_y;
    uint32_t hp;
    uint32_t max_hp;
    // ... discovered fields
};
""")

# Apply types to variables
mcp_ida-pro-mcp_set_local_variable_type(
    function_address="0x404CD0",  # UPDATE_GAME_STATE_ADDR
    variable_name="a1",
    new_type="PlayerState*"
)
```

---

**Status**: 🚀 **FM2K REVERSE ENGINEERING - IN PROGRESS** 🎯  
**Project**: Fighter Maker 2nd fighting game engine analysis  

**CURRENT TARGET**: 
- 🔄 **FM2K game executables** (e.g., WonderfulWorld_ver_0946.exe) - FUNCTION RENAMING IN PROGRESS
  - ✅ Core hook points identified (FRAME_HOOK_ADDR, UPDATE_GAME_STATE_ADDR)
  - ✅ Timing system mapped (100 FPS = 10ms per frame, CRITICAL for rollback!)
  - ✅ Player state structure mapped (from FM2K_Integration.h)
  - ✅ Input system analyzed (11-bit mask, 1024-frame history)
  - ✅ RNG system identified (RANDOM_SEED_ADDR @ 0x41FB1C)
  - ✅ Memory addresses mapped (player HP, position, input buffers)
  - ⚠️ Many `sub_` functions still need renaming
  - ⚠️ Object pool structure needs complete mapping
  - ⚠️ Effect system needs deeper analysis

**Key Systems Identified**:
- Frame processing system (100 FPS = 10ms per frame, CRITICAL for rollback!)
- Player state system (HP, position, input history)
- Input system (11-bit mask, 1024-frame history buffer)
- RNG system (must save/restore for rollback determinism)
- Object pool system (OBJECT_POOL_ADDR @ 0x4701E0)
- Effect system (active flags, timers, colors, targets)
- Rendering and sprite system
- UI and menu system

**Last Updated**: 2025-01-XX  
**Next Focus**: Complete object pool mapping, continue renaming remaining `sub_` functions, analyze effect system

---

## 🎮 **FM2K Game Systems Analysis**

### Main Game Loop Structure

**Frame Processing Flow:**
- **FRAME_HOOK_ADDR** @ 0x4146D0 - Main frame processing function
- **UPDATE_GAME_STATE_ADDR** @ 0x404CD0 - Game state update function
- **100 FPS** - Fixed frame rate (10ms per frame)

**Main Loop Structure:**
```c
// Main game loop (100 FPS = 10ms per frame)
while (ShouldContinueEventLoop()) {
    // Process frame (FRAME_HOOK_ADDR @ 0x4146D0)
    ProcessFrame();
    
    // Update game state (UPDATE_GAME_STATE_ADDR @ 0x404CD0)
    UpdateGameState();
    
    // Update input buffer index
    g_input_buffer_index = (g_input_buffer_index + 1) % 1024;
    
    // Wait for next frame (10ms)
    WaitForNextFrame();
}
```

### Menu State Machine Flow (ProcessMainMenuStateMachine @ 0x48CA00)

**Exit Code Cases (`g_main_menu_state` / `g_exit_result_code` @ 0x4AB8C4):**
- **Case 0 (INIT)**: 
  - Initializes intro movie pixel arrays (`g_intro_movie_pixel_count`)
  - Resets `g_intro_movie_initialized = 0`, `g_menu_init_flag = 0`
  - **CRITICAL DECISION**: `g_main_menu_state = g_system_check_flags != 0 ? 1 : 3`
    - If `system_check_flags != 0`: Sets state to **1 (STATISTICS)** → **Opening.avi will play, then stats screen!**
    - If `system_check_flags == 0`: Sets state to **3 (MENU_SETUP)** → **Opening.avi is SKIPPED, goes straight to main menu!**
  - Returns 1

- **Case 1 (STATISTICS)**: 📊 **STATISTICS SCREEN (shows AFTER intro movie)**
  - Calls `ProcessIntroMovie()` @ 0x4865A0
  - **IMPORTANT**: This case name is misleading! "STATISTICS" refers to the **stats screen that appears AFTER Opening.avi completes**
  - `ProcessIntroMovie()` handles BOTH:
    1. **Opening.avi playback** (via `ProcessGameUI_Thunk` callback)
    2. **Statistics screen display** (after intro completes)
  - When intro completes or is skipped, sets `g_main_menu_state = 3` (MENU_SETUP)
  - Returns result from ProcessIntroMovie()
  
  **NOTE**: Opening.avi plays FIRST (within ProcessIntroMovie), then the statistics screen appears. The case is named "STATISTICS" because that's what you see after the intro!

- **Case 2 (MAIN_GAME)**: 
  - Calls `ProcessMainMenu()` @ 0x486C90 (main menu screen handler)
  - Shows main menu with options: 1P vs 2P, VS Mode, OPTION, EXIT
  - Returns result from ProcessMainMenu()

- **Case 3 (MENU_SETUP)**: 
  - Finalizes intro movie render buffers
  - Sets up palette data
  - **CRITICAL**: Calls `LoadStageBackground(255, 0)` - loads InitAppMain resources
  - Sets `g_intro_movie_initialized = 0`
  - Sets `g_main_menu_state = 2` (MAIN_GAME)
  - Returns 1

- **Case 4 (CSS_SETUP)**: ⚠️ **AVOID!** 
  - Sets `game_state=2` (BATTLE), `g_battle_type = 1` (DEMO MODE!)
  - Resets `g_main_menu_state = 0` (INIT)
  - Causes demo mode battle - DO NOT USE for boot-to-CSS!

- **Case 5 (CSS_ACTIVE)**: ✅ **TARGET FOR BOOT-TO-CSS!**
  - Calls `ProcessCharacterSelectScreen()` @ 0x487C50
  - This is the actual character selection screen
  - Returns result from ProcessCharacterSelectScreen()

- **Case 6**: 
  - Sets up stage/network config
  - Sets `g_main_menu_state = 5` (CSS_ACTIVE)
  - Loads stage background (34)
  - Returns 1

- **Case 7-14**: Various stats/result screens (not relevant for boot flow)

**Boot Flow Summary:**
```
Boot → Case 0 (INIT)
  ├─ system_check_flags != 0 → Case 1 (STATISTICS) → ProcessIntroMovie()
  │   ├─ Opening.avi plays (via ProcessGameUI_Thunk)
  │   ├─ Statistics screen appears (after intro)
  │   └─ After stats: Case 3 (MENU_SETUP) → Case 2 (MAIN_GAME)
  └─ system_check_flags == 0 → Case 3 (MENU_SETUP) → Case 2 (MAIN_GAME) [Opening.avi + stats SKIPPED]
```

### Character Select State Machine (ProcessCharacterSelectStateMachine @ 0x428430)

**Purpose**: Handles character selection screen state machine (was incorrectly called "ProcessGameState2")

**State Cases (`g_character_select_state`):**
- **Case -1**: Initialize character selection
  - Sets up player flags, controller types, character IDs
  - Handles costume selection and stage selection
  - Loads character assets and initializes battle system
  - Sets `g_character_select_state = 0` on success
- **Case 0**: Battle mode active (`ProcessBattleLoop` @ 0x421ee0) ✅ **VERIFIED**
  - **Decompiled Code**: `case 0: game_state_result = ProcessBattleLoop();` (line 0x4289eb)
  - **NOTE**: Originally named `RenderDebugCharacterInfo` but renamed to `ProcessBattleLoop` because it's the main battle loop
  - Called every frame during battle gameplay
  - Processes character logic, collision, rendering, etc.
- **Case 1**: Process character select (`ProcessCharacterSelect`) ✅ **VERIFIED**
  - **Decompiled Code**: `case 1: ProcessCharacterSelect();` (line 0x4289f2)
- **Case 2**: Render stage selection UI (`RenderStageSelectionUI` @ 0x404f90) ✅ **VERIFIED**
  - **Decompiled Code**: `case 2: game_state_result = RenderStageSelectionUI();` (line 0x4289f9)
  - **What It Actually Does** (from decompiled function):
    - Renders match statistics: win status, win count, combo count, music volume
    - Checks for input to skip victory screen
    - Sets `g_character_select_state = 5` when player skips (line 0x407eb6)
  - **NOTE**: Function name is misleading - it's the POST_MATCH_VICTORY screen, not stage selection
  - Called after battle ends to show victory screen with match stats
- **Case 3**: Process loading screen (`ProcessLoadingScreen`)
- **Case 4**: Process versus screen (`ProcessVersusScreen`)
  - Shows "Fight!" intro before battle starts
- **Case 5**: Check input debug mode transition (`CheckInputDebugModeTransition` @ 0x408180) ✅ **VERIFIED**
  - **Decompiled Code**: `case 5: game_state_result = CheckInputDebugModeTransition();` (line 0x428a0e)
  - **What It Actually Does** (from decompiled function):
    - Calls `InitializeAudioSystem()`
    - Can set state to -1 (INIT): `g_character_select_state = -1` (lines 0x4081db, 0x4082c3)
    - Can set state to 3 (LOADING): `g_character_select_state = 3` (line 0x408288)
    - Returns 1 to continue processing
  - **NOTE**: This is a TRANSITION state, not a scene
  - **Purpose**: Transition check after victory screen - checks conditions and transitions to appropriate state
  - **Ephemeral**: State 5 is very brief - it immediately transitions to another state

**Return Values:**
- `-2`: Character initialization failed
- `-1`: App initialization failed
- `0`: Continue to state 3 (character selection flow)
- `1`: Continue processing
- `2`: Exit character select (confirmed)
- `3`: Exit to main menu
- `4`: Continue to state 4 (battle)

### Character Selection Flow (ProcessCharacterSelectionFlow @ 0x43a790)

**Purpose**: Handles character selection flow state machine (was incorrectly called "ProcessGameState3")

**State Cases (`g_game_state_3_machine`):**
- **Case 0**: Initialize character select phase 2 (`InitializeCharacterSelectPhase2`)
- **Case 1**: Process versus mode selector (`ProcessVersusModeSelector`) - VS menu (1P vs COM, 1P vs 2P selection)
- **Case 2**: Process battle screen and character selection (`ProcessBattleScreenAndCharacterSelection`)
- **Case 3**: Initialize character select mode (`InitializeCharacterSelectMode`)

**Return Values:**
- `0`: Exit (error)
- `1`: Continue processing
- `2`: Transition to battle (confirmed selection)
- `3`: Exit to main menu

### Boot-to-CSS Implementation Strategy

**Goal**: Jump directly to character select screen on boot

**Method 1: Route through Main Menu State Machine**
1. Let MENU_SETUP (case 3) run normally - initializes InitAppMain via `LoadStageBackground(255, 0)`
2. Intercept transition from MENU_SETUP → MAIN_GAME
3. Route directly to case 5 (CSS_ACTIVE) instead of MAIN_GAME
4. Set `g_current_game_state = 2` (CSS state) - required for ProcessCharacterSelectStateMachine
5. Set `character_select_mode = 1` (1v1), `battle_mode_type = 1`, `local_mode = 1`
6. Clear player input flags to prevent auto-selection

**Method 2: Direct State Jump (Recommended)**
1. Set `g_current_game_state = 2` (Character select screen)
2. Set `g_previous_game_state = 1` (Main menu - for proper state transition)
3. Initialize CSS state machine:
   - Set `g_character_select_state = 1` (Process character select)
   - Set `g_game_state_3_machine = 2` (Process battle screen and character selection)
4. Set required flags:
   - `g_character_select_mode = 1` (1v1 mode)
   - `g_battle_mode_type = 1`
   - `g_local_mode_flag = 1`
   - `g_player_input_flags = 0` (prevent auto-selection)

**Key Variables:**
- `g_current_game_state` (0x61801C): Current game state (2 = CSS)
- `g_previous_game_state` (0x6234EC): Previous game state (for state transitions)
- `g_character_select_state` (unknown): Character select state machine index
- `g_game_state_3_machine` (unknown): Character selection flow state machine index
- `g_exit_result_code` (0x4AB8C4): Menu state machine exit code
- `g_character_select_mode` (0x6236B0): 1 = 1v1, 2 = 2v2
- `g_battle_mode_type` (0x6236B0): Battle mode type
- `g_player_input_flags` (0x6236A8): Player input flags (must be 0 to prevent auto-selection)

**Functions:**
- `ProcessMainMenuStateMachine()` @ 0x48CA00: Main menu state machine
- `ProcessCharacterSelectStateMachine()` @ 0x428430: Character select screen state machine (was "ProcessGameState2")
- `ProcessCharacterSelectionFlow()` @ 0x43a790: Character selection flow state machine (was "ProcessGameState3")
- `ProcessBattleScreenAndCharacterSelection()` @ 0x434c80: Battle screen and character selection handler
- `ProcessVersusModeSelector()` @ 0x439f70: Versus mode selector (1P vs COM, 1P vs 2P)
- `ProcessCharacterSelectionAndRendering()` @ 0x487C50: Character select screen rendering
- `ProcessMainGameLoop()` @ 0x486C90: Main menu handler (case 2)
- `ProcessIntroMovie()` @ 0x4865A0: Intro movie (case 1, renamed from ProcessStatisticsScreen)

### Function Naming Improvements

**Bad Names → Better Names:**

1. **`ProcessGameState3` @ 0x43a790** → **`ProcessCharacterSelectionFlow`** ✅ **CORRECTED**
   - **Why the old name is bad**: "GameState3" is meaningless - what is state 3?
   - **What it actually does**: Character selection flow state machine that handles:
     - Case 0: InitializeCharacterSelectPhase2
     - Case 1: ProcessVersusModeSelector (VS menu - 1P vs COM, 1P vs 2P selection)
     - Case 2: ProcessBattleScreenAndCharacterSelection
     - Case 3: InitializeCharacterSelectMode
   - **Correct name**: `ProcessCharacterSelectionFlow` (describes the flow of character selection)
   - **Note**: This is called when `g_current_game_state == 3` in winMain

2. **`ProcessGameState2`** → **`ProcessCharacterSelectStateMachine`** ✅ **ALREADY CORRECTLY NAMED**
   - **What it actually does**: Character select screen state machine (handles CSS state transitions)
   - **State cases**: 
     - -1 (INIT_SCENE - initialization)
     - 0 (BATTLE_SCENE - `ProcessBattleLoop` - battle gameplay)
     - 1 (CHARACTER_SELECT_SCENE - `ProcessCharacterSelect` - actual CSS)
     - 2 (POST_MATCH_VICTORY_SCENE - `RenderStageSelectionUI` - victory screen)
     - 3 (LOADING_SCENE - `ProcessLoadingScreen`)
     - 4 (VERSUS_INTRO_SCENE - `ProcessVersusScreen` - "Fight!" intro)
     - 5 (POST_MATCH_TRANSITION - `CheckInputDebugModeTransition` - transition check, not a scene)
   - **Correct name**: `ProcessCharacterSelectStateMachine` @ 0x428430 (already correctly named in IDA)
   - **Note**: This is called when `g_current_game_state == 2` in winMain
   - **Important**: State 0 was originally called `RenderDebugCharacterInfo` but renamed to `ProcessBattleLoop` because that's what it actually does

3. **`ProcessSpecialMode` @ 0x41e410** → **`FlushCommandQueue`** ✅ **CORRECTED**
   - **Why the old name is bad**: "SpecialMode" is vague - what special mode?
   - **What it actually does**: Executes command queue based on execution mode flag:
     - If `dword_6267D4 == 1`: Calls `ExecuteRenderCommandQueue`
     - Otherwise: Calls `ExecuteCallbackCommandQueue`
     - Resets the flag after execution
   - **Called by**: ProcessCharacterSelect, RenderStageSelectionUI, ProcessLoadingScreen, ProcessVersusScreen, FlushGraphicsContext, winMain, ProcessIntroMovie
   - **Correct name**: `FlushCommandQueue` (clear and concise - flushes/executes pending command queue)

**Naming Principle**: Function names should describe **what they do**, not **where they are** (like "State3") or **how they're categorized** (like "SpecialMode"). The name should make the function's purpose immediately clear to someone reading the code.

**State Machine Hierarchy:**
```
winMain (main loop)
  ├─ State 1: ProcessMainMenuStateMachine (main menu)
  ├─ State 2: ProcessCharacterSelectStateMachine (CSS state machine)
  │   └─ Calls ProcessCharacterSelectionFlow when transitioning
  ├─ State 3: ProcessCharacterSelectionFlow (character selection flow)
  │   ├─ Case 0: InitializeCharacterSelectPhase2
  │   ├─ Case 1: ProcessVersusModeSelector
  │   ├─ Case 2: ProcessBattleScreenAndCharacterSelection
  │   └─ Case 3: InitializeCharacterSelectMode
  └─ State 4: Battle/versus mode
```


