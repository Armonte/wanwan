# 2DFM Object/Script System Analysis

## Overview

2DFM (2D Fighting Maker) uses a sophisticated **hierarchical script-based system** for managing game objects, visual elements, and interactive behaviors. Unlike traditional object-oriented game engines, 2DFM employs a **visual scripting approach** where game logic is defined through script commands rather than direct object manipulation.

## Core Architecture

### **1. Script System Structure**

The foundation of 2DFM's object system is built around **scripts** and **script items**:

```cpp
// Script management (from 2dfmCommon.hpp)
struct Script {
    char scriptName[32];        // Script identifier
    std::uint16_t scriptIndex;  // Index in script array
    byte gap;                   // Padding
    int32_t flags;              // Script type flags
};

// Script items (16 bytes each)
struct ScriptItem {
    byte type;                  // Command type
    byte bytes[15];             // Command-specific data
};
```

**Key Characteristics:**
- **Fixed Sizes**: Scripts are 39 bytes, script items are 16 bytes
- **Hierarchical Organization**: Scripts contain multiple script items
- **Type-Based**: Each script item has a specific command type
- **Flag-Based Categorization**: Scripts use flags for special categorization

### **2. Script Special Flags System**

2DFM uses an extensive flag-based system to categorize scripts by their purpose:

```cpp
enum class ScriptSpecialFlag {
    NORMAL = 0,                 // General scripts
    BACKGROUND = 1,             // Background elements, cursors
    SYSTEM = 3,                 // System UI elements
    STAGE_MAIN_UI = 9,         // Main UI (HP bars, skill bars)
    COMBO_SYMBOL = 33,         // Combo number displays
    ROUND = 57,                // Round start/end animations
    TIME_NUMBER = 65,          // Timer displays
    HIT_SYMBOL = 97,           // Hit combo symbols
    SKILL_POINT_NUMBER = 129,  // Skill point numbers
    VICTORY_FLAG = 193,        // Victory indicators
    TIMER_POS = 131,           // Timer position
    PLAYER_1_AVATAR_POS = 195, // Player 1 avatar position
    PLAYER_2_AVATAR_POS = 259, // Player 2 avatar position
    PLAYER_1_SKILL_POINT_POS = 323, // Player 1 skill point position
    PLAYER_2_SKILL_POINT_POS = 387, // Player 2 skill point position
    PLAYER_1_VICTORY_POS = 451, // Player 1 victory position
    PLAYER_2_VICTORY_POS = 515, // Player 2 victory position
};
```

**Flag Usage Patterns:**
- **UI Elements**: BACKGROUND, SYSTEM, STAGE_MAIN_UI
- **Gameplay Elements**: ROUND, TIME_NUMBER, COMBO_SYMBOL
- **Player-Specific**: PLAYER_1_*, PLAYER_2_* variants
- **Position Markers**: *_POS flags for coordinate storage

### **3. Script Command Types**

The system supports **15 different command types** for visual scripting:

```cpp
enum class CommonScriptItemTypes {
    START = 0,      // Initialize script execution
    MOVE = 1,       // Movement commands
    SOUND = 3,      // Audio playback
    OBJECT = 4,     // Object creation and management
    END = 5,        // Terminate script execution
    LOOP = 9,       // Looping constructs
    JUMP = 10,      // Jump to different script
    CALL = 11,      // Call sub-script
    PIC = 12,       // Display picture/sprite
    COLOR = 35,     // Color effects and blending
    VARIABLE = 31,  // Variable operations
    RANDOM = 32,    // Random branching logic
    AFTERIMAGE = 37 // Visual effects (afterimages)
};
```

**Command Categories:**
- **Flow Control**: START, END, JUMP, CALL, LOOP
- **Visual**: PIC, COLOR, AFTERIMAGE
- **Audio**: SOUND
- **Logic**: VARIABLE, RANDOM
- **Movement**: MOVE
- **Object Management**: OBJECT

## Advanced Command Structures

### **4. Object Command System**

The most complex command type is the **Object Command**, which manages game entities:

```cpp
struct ObjectCmd {
    byte type;                  // Command type (4 for OBJECT)
    byte flags;                 // Object behavior flags
    uint16_t targetScriptId;    // Target script for object
    uint8_t targetPos;          // Position in target script
    uint16_t targetScriptIdIfExists; // Alternative script
    uint8_t targetPosIfExists;  // Alternative position
    int16_t posX, posY;         // Object position
    uint8_t manageNo;           // Management number (0-9)
    int8_t layer;               // Render layer (0-127)
    
    // Layer system implementation
    int getLayer() const {
        switch (flags & 0b0011) {
        case 0: return 0;           // Default layer
        case 1: return 127;         // Top layer
        case 2: default: return layer; // Custom layer
        }
    }
    
    // Object behavior flags
    bool isAttachAsChild() const { return flags & 0b00100000; }
    bool isUnconditionally() const { return flags & 0b0100; }
    bool isShowShadow() const { return flags & 0b1000; }
    bool isUseWindowPosition() const { return flags & 0b01000000; }
    
    // Management number logic
    int getManageNo() const {
        if (isUnconditionally()) return -1;
        if (manageNo >= 0 && manageNo < 10) return manageNo;
        return -1;
    }
};
```

**Layer System:**
- **70-80**: Character layers
- **>80**: Display in front of characters
- **<70**: Display behind characters
- **0**: Default layer
- **127**: Top layer

### **5. Variable System**

2DFM implements a sophisticated variable system with different scopes:

```cpp
struct VariableCmd {
    /*
     * Variable addressing scheme:
     * 00-0F: Task variables A-P
     * 40-4F: Character variables A-P (character scripts only)
     * 80-8F: System variables A-P
     * C0-C7: Special values (coordinates, time, round count)
     *  C0: X coordinate
     *  C1: Y coordinate
     *  C2: Map X coordinate
     *  C3: Map Y coordinate
     *  C4: Parent X coordinate
     *  C5: Parent Y coordinate
     *  C6: Time
     *  C7: Round count
     */
    
    byte targetVariable;        // Target variable address
    byte opFlags;               // Operation flags
    byte compareVariable;       // Comparison variable
    int16_t operationValue;     // Assignment/addition value
    int16_t compareValue;       // Comparison value
    
    // Operation types
    enum class Operation {
        NONE, ASSIGNMENT, ADDITION,
    };
    
    enum class Condition {
        NONE, EQUALS, LESS, GREATER,
    };
    
    Operation getOperation() const {
        int v = opFlags & 0b0011;
        switch (v) {
        case 0: return Operation::NONE;
        case 1: return Operation::ASSIGNMENT;
        case 2: default: return Operation::ADDITION;
        }
    }
    
    Condition getCondition() const {
        int v = opFlags & 0b00001100;
        switch (v) {
        case 0: return Condition::NONE;
        case 4: return Condition::EQUALS;
        case 8: return Condition::GREATER;
        case 12: default: return Condition::LESS;
        }
    }
};
```

### **6. Movement and Position Commands**

```cpp
struct KgtPos {
    byte type;
    int16_t x;
    int16_t y;
    
    ax::Vec2 getPosition() const { return ax::Vec2(x, y); }
};

struct KgtPosAndOffset : KgtPos {
    signed char offsetX;
    signed char offsetY;
    
    ax::Vec2 getOffset() const { return ax::Vec2(offsetX, offsetY); }
};

struct MoveCmd {
    byte type;
    int16_t accelX;
    int16_t moveX;
    int16_t moveY;
    int16_t accelY;
    byte flags;
    
    bool isAdd() const { return flags & 0x1; }
    bool isIgnoreMoveX() const { return flags & 0x2; }
    bool isIgnoreMoveY() const { return flags & 0x4; }
    bool isIgnoreAccelX() const { return flags & 0x8; }
    bool isIgnoreAccelY() const { return flags & 0x16; }
};
```

### **7. Visual Effects Commands**

```cpp
struct ShowPic {
    byte type;                  // 0
    uint16_t keepTime;          // 1: Display duration
    uint16_t idxAndFlip;        // 3: Picture index and flip flags
    int16_t offsetX;            // 5: X offset
    int16_t offsetY;            // 7: Y offset
    byte fixDir;                // 9: Direction fix
    
    int getPicIdx() const { return idxAndFlip & 0x3fff; }
    bool isFlipX() const { return (idxAndFlip & 0x4000) != 0; }
    bool isFlipY() const { return (idxAndFlip & 0x8000) != 0; }
    ax::Vec2 getOffset() const { return ax::Vec2(offsetX, offsetY); }
};

struct ColorSetCmd {
    byte type;
    byte colorBlendType;
    int8_t red, green, blue, alpha;
    
    ColorBlendType getColorBlendType() const {
        return static_cast<ColorBlendType>(colorBlendType);
    }
};

enum class ColorBlendType {
    NORMAL = 0,
    TRANSPARENCY,
    ADD_BLEND,
    MINUS_BLEND,
    ALPHA_BLEND,
};
```

## Resource Management System

### **8. Picture/Texture System**

```cpp
struct Picture {
    PictureHeader header;       // 20 bytes metadata
    byte *content;              // Image data
};

struct PictureHeader {
    int unknownFlag1;           // 0-3
    int width, height;          // 4-11: Dimensions
    int hasPrivatePalette;      // 12-15: Private vs shared palette
    int size;                   // 16-19: Compressed size (0 = uncompressed)
};

// Color representation
union ColorBgra {
    struct {
        byte blue, green, red, alpha;
    } channel;
    std::uint32_t value;
    
    std::string toString() {
        return std::format("0x{:2X}{:2X}{:2X}{:2X}",
            (int)channel.red,
            (int)channel.green,
            (int)channel.blue,
            (unsigned char)channel.alpha == 0 ? 0 : 255);
    }
};
```

**Palette System:**
- **8 Shared Palettes**: Available to all pictures
- **Private Palettes**: Per-picture custom palettes
- **256 Colors**: Each palette contains 256 color entries
- **BGRA Format**: Blue-Green-Red-Alpha color format

### **9. Sound System**

```cpp
enum class SoundType {
    WAVE = 1,
    MIDI,
    CDDA
};

struct SoundItemHeader {
    int unknown;                // 0-3
    char name[32];              // 4-35: Sound name
    int size;                   // 36-39: Sound data size
    byte soundType;             // 40: Sound type
    byte track;                 // 41: Track number
    
    SoundType getSoundType() {
        return static_cast<SoundType>(soundType & 0b1111);
    }
    
    bool isLoop() {
        return soundType & 0b10000;
    }
};

struct Sound {
    SoundItemHeader header;
    byte *content;
};
```

### **10. File Format Structure**

#### **KGT File Format**
```cpp
struct KgtFileHeader {
    byte fileSignature[16];     // File identifier
    NameInfo name;              // 256-byte name
};

struct CommonResourcePart {
    int scriptCount;            // Number of scripts
    byte *rawScriptsData;       // Script definitions
    int scriptItemCount;        // Number of script items
    ScriptItem *scriptItems;    // Script commands
    int pictureCount;           // Number of pictures
    std::vector<Picture *> pictures;
    ColorBgra *sharedPalettes[8];
    int soundCount;
    std::vector<Sound *> sounds;
};
```

#### **Resource Loading Process**
```cpp
// From 2dfmFileReader.cpp
void readScripts(CommonResource *result, byte *rawData, int scriptCount, int itemCount) {
    for (auto i = 0; i < scriptCount; ++i) {
        auto s = reinterpret_cast<_2dfm::Script *>(rawData + i * _2dfm::SCRIPT_SIZE);
        int endIndex = 0;
        if (i == scriptCount - 1) {
            endIndex = itemCount;
        } else {
            auto nextS = reinterpret_cast<_2dfm::Script *>(rawData + (i + 1) * _2dfm::SCRIPT_SIZE);
            endIndex = nextS->scriptIndex;
        }
        KgtScript kgtScript {
            static_cast<ScriptSpecialFlag>(s->flags),
            gbkToUtf8(s->scriptName),
            static_cast<int>(s->scriptIndex),
            endIndex
        };
        result->scripts.emplace_back(kgtScript);
    }
}
```

## State Management for Rollback

### **11. Critical State Components**

For effective rollback implementation, the following state must be preserved:

#### **Script Execution State**
```cpp
struct ScriptExecutionState {
    uint32_t currentScriptId;       // Currently executing script
    uint32_t currentScriptPos;      // Position within script
    uint32_t scriptStack[16];       // Call stack for nested scripts
    uint32_t stackPointer;          // Current stack depth
    uint32_t loopCounters[8];       // Loop iteration counters
    uint32_t loopTargets[8];        // Loop target positions
};
```

#### **Variable State**
```cpp
struct VariableState {
    int16_t taskVariables[16];      // Task variables A-P (00-0F)
    int16_t characterVariables[16]; // Character variables A-P (40-4F)
    int16_t systemVariables[16];    // System variables A-P (80-8F)
    int16_t specialValues[8];       // Special values C0-C7
};
```

#### **Object State**
```cpp
struct ObjectState {
    uint32_t objectId;              // Object identifier
    uint32_t scriptId;              // Associated script
    uint32_t positionX, positionY;  // Object position
    uint32_t layer;                 // Render layer
    uint32_t manageNo;              // Management number
    uint32_t flags;                 // Object flags
    uint32_t animationFrame;        // Current animation frame
    uint32_t animationTimer;        // Animation timing
};
```

### **12. Performance Optimizations**

#### **Differential State Saving**
```cpp
struct DifferentialState {
    uint32_t changedScripts[32];    // Bitmap of changed scripts
    uint32_t changedVariables[8];   // Bitmap of changed variables
    uint32_t changedObjects[32];    // Bitmap of changed objects
    std::vector<uint8_t> scriptDiffs;   // Script state differences
    std::vector<uint8_t> variableDiffs; // Variable differences
    std::vector<uint8_t> objectDiffs;   // Object state differences
};
```

#### **State Compression**
- **Script State**: Only save current position and stack
- **Variable State**: Only save non-zero variables
- **Object State**: Only save active objects
- **Resource State**: Cache loaded resources

### **13. Rollback Implementation Strategy**

#### **State Serialization**
```cpp
struct GameState {
    // Script system state
    ScriptExecutionState scriptState;
    VariableState variableState;
    
    // Object system state
    std::vector<ObjectState> activeObjects;
    
    // Resource state
    std::vector<uint32_t> loadedPictures;
    std::vector<uint32_t> loadedSounds;
    
    // Game state
    uint32_t gameMode;
    uint32_t roundNumber;
    uint32_t timerValue;
    uint32_t randomSeed;
};
```

#### **State Restoration**
1. **Restore Script State**: Resume script execution from saved position
2. **Restore Variables**: Load all variable values
3. **Restore Objects**: Recreate active objects with saved state
4. **Restore Resources**: Ensure required resources are loaded
5. **Restore Game State**: Set game mode, timer, etc.

## Comparison with Other Systems

### **14. 2DFM vs FM2K Comparison**

| Aspect | 2DFM | FM2K |
|--------|------|------|
| **Object System** | Script-based, hierarchical | Fixed pool (1024 objects) |
| **State Size** | Variable (scripts + resources) | Fixed (390KB object pool) |
| **Complexity** | High (script interpretation) | Medium (direct object updates) |
| **Rollback Difficulty** | High (script state + resources) | Medium (object pool snapshots) |
| **Flexibility** | High (dynamic scripting) | Low (fixed object types) |
| **Performance** | Variable (script execution overhead) | Consistent (direct updates) |
| **Memory Usage** | Dynamic (resource loading) | Fixed (pre-allocated pool) |

### **15. 2DFM vs Giuroll Comparison**

| Aspect | 2DFM | Giuroll (Touhou 12.3) |
|--------|------|----------------------|
| **State Management** | Script interpretation | Memory snapshots |
| **State Size** | Variable (10-100KB typical) | Fixed (~50KB per frame) |
| **Rollback Approach** | Script state restoration | Memory restoration |
| **Performance** | Script execution overhead | Direct memory operations |
| **Complexity** | High (script system) | Medium (memory management) |

## Implementation Recommendations

### **16. Rollback Integration Points**

#### **Script Execution Hook**
```cpp
// Hook script execution to capture state
void hookScriptExecution(uint32_t scriptId, uint32_t position) {
    // Save current script state
    saveScriptState(scriptId, position);
    
    // Execute script
    executeScript(scriptId, position);
}
```

#### **Variable Access Hook**
```cpp
// Hook variable access for state tracking
int16_t hookVariableAccess(uint32_t variableId, int16_t value) {
    // Track variable changes
    trackVariableChange(variableId, value);
    
    return value;
}
```

#### **Object Creation Hook**
```cpp
// Hook object creation for state management
uint32_t hookObjectCreation(uint32_t scriptId, uint32_t position) {
    // Track object creation
    trackObjectCreation(scriptId, position);
    
    return createObject(scriptId, position);
}
```

### **17. Performance Considerations**

#### **State Size Optimization**
- **Script State**: ~1KB per active script
- **Variable State**: ~128 bytes (16 variables Å~ 4 scopes Å~ 2 bytes)
- **Object State**: ~64 bytes per active object
- **Resource State**: Cached, not saved per frame

#### **Execution Overhead**
- **Script Interpretation**: ~1-5ms per frame
- **State Saving**: ~0.1-1ms per frame
- **State Restoration**: ~1-10ms per frame

#### **Memory Management**
- **Resource Caching**: Pre-load commonly used resources
- **State Compression**: Use differential saving
- **Garbage Collection**: Clean up unused resources

### **18. Debugging and Development**

#### **State Visualization**
```cpp
struct StateDebugInfo {
    std::vector<std::string> activeScripts;
    std::vector<std::string> activeObjects;
    std::map<uint32_t, int16_t> variableValues;
    std::vector<std::string> loadedResources;
};
```

#### **State Validation**
```cpp
bool validateGameState(const GameState& state) {
    // Validate script state consistency
    if (!validateScriptState(state.scriptState)) return false;
    
    // Validate object state consistency
    if (!validateObjectState(state.activeObjects)) return false;
    
    // Validate resource availability
    if (!validateResources(state.loadedPictures, state.loadedSounds)) return false;
    
    return true;
}
```

## Conclusion

2DFM's object/script system represents a sophisticated approach to game development that prioritizes **flexibility and visual programming** over raw performance. While this makes rollback implementation more complex than traditional object pool systems, it provides significant advantages in terms of **modularity, reusability, and ease of content creation**.

The key challenges for rollback implementation are:
1. **Script State Management**: Preserving execution context across frames
2. **Variable State Tracking**: Maintaining consistent variable values
3. **Resource State Management**: Ensuring proper resource loading
4. **Performance Optimization**: Minimizing overhead of script interpretation

With careful implementation of state serialization and restoration, 2DFM can achieve effective rollback netcode while maintaining its powerful scripting capabilities. 