# FM2K System Improvements for Rollback Netcode

This document outlines key architectural improvements for integrating GekkoNet rollback, focusing on robust state management inspired by solutions for other legacy fighting games like `giuroll` for EFZ.

---

## 1. GekkoNet Integration & Game Loop

The existing plan in `FM2K_Rollback_Plan.md` aligns well with GekkoNet's event-driven model. We will replace our manual frame-stepping logic with a loop that is driven by events from `gekko_update_session()`.

### Proposed Game Loop (`FM2K_GameInstance.cpp`)

```cpp
// In FM2K_GameInstance::RunGameLoop() or similar
void FM2KGameInstance::RunGameLoop() {
    while (is_running_) {
        // 1. Collect local inputs and send to GekkoNet
        CollectLocalInputs(); // Reads FM2K input state
        gekko_add_local_input(session_, local_player_handle_, &last_collected_input_);

        // 2. Poll network events (if using the default adapter)
        gekko_network_poll(session_);

        // 3. Process GekkoNet events
        int event_count = 0;
        GekkoGameEvent** events = gekko_update_session(session_, &event_count);

        for (int i = 0; i < event_count; ++i) {
            GekkoGameEvent* event = events[i];
            switch (event->type) {
                case SaveEvent:
                    HandleSaveEvent(&event->data.save);
                    break;
                case LoadEvent:
                    HandleLoadEvent(&event->data.load);
                    break;
                case AdvanceEvent:
                    HandleAdvanceEvent(&event->data.adv);
                    break;
            }
        }

        // 4. Handle session events (connects, disconnects, etc.)
        ProcessSessionEvents();
        
        // 5. Render the frame (this can run independently of the logic update)
        RenderGame();
    }
}
```

### Event Handlers

-   **`HandleSaveEvent(GekkoGameEvent::Save* event)`**: This function will be responsible for serializing the entire game state into the `event->state` buffer provided by GekkoNet. It will call our new `GameStateManager`.
-   **`HandleLoadEvent(GekkoGameEvent::Load* event)`**: This will deserialize the state from `event->state` and restore the game's memory.
-   **`HandleAdvanceEvent(GekkoGameEvent::Advance* event)`**: This will take the confirmed `event->inputs`, place them in the correct memory locations for `P1` and `P2`, and then call the original `update_game_state` function (`0x404CD0`).

---

## 2. Advanced State Management: The `GameStateManager`

The biggest challenge is ensuring 100% deterministic state serialization. Relying on a static list of memory addresses is brittle. `giuroll`'s success with EFZ highlights the need for a more comprehensive approach, especially regarding the heap.

We will create a `GameStateManager` to centralize all state operations.

### Core Responsibilities:

1.  **Static Memory Snapshot**: Save and load the known critical global memory regions identified in our research docs (object pools, game state variables, etc.).
2.  **Heap Management**: Intercept and manage dynamic memory allocations to ensure they are part of the state snapshot.

### Heap Management Strategy: Arena Allocator

Instead of trying to snapshot the process heap, we will replace FM2K's memory allocation functions (`malloc`, `free`, `new`, `delete`, and their variants) with our own using MinHook.

**How it works:**

1.  **Hook Allocators**: On startup, we will hook all of FM2K's memory allocation and deallocation functions.
2.  **Create an Arena**: We will pre-allocate a large, fixed-size block of memory (e.g., 8-16MB) to serve as our "arena".
3.  **Redirect Allocations**: Our hooked `malloc`/`new` will grab memory from this arena using a simple bump allocator. Our hooked `free`/`delete` will do nothing, as the entire arena is cleared at once.
4.  **State Save/Load**:
    *   **To Save**: The `GameStateManager` copies the *entire arena* into the GekkoNet state buffer. This captures all dynamically allocated game objects in a single, fast `memcpy`.
    *   **To Load**: The `GameStateManager` copies the saved arena state back, instantly restoring all dynamic objects.

This approach bypasses the complexity of tracking individual allocations and pointers, making state management significantly more robust and deterministic.

### Implementation Sketch (`GameStateManager.h`)

```cpp
#pragma once

#include <vector>
#include <cstdint>
#include "gekkonet.h"

// List of static memory regions to save.
struct MemoryRegion {
    uintptr_t start;
    size_t size;
};

class GameStateManager {
public:
    GameStateManager(size_t arena_size_bytes);
    ~GameStateManager();

    // Hooks FM2K's memory functions to use our arena.
    bool InstallHeapHooks();
    void UninstallHeapHooks();

    // Called by our GekkoNet event handlers.
    void SaveState(GekkoGameEvent::Save* event);
    void LoadState(GekkoGameEvent::Load* event);

private:
    // Our private memory heap for the game.
    std::vector<uint8_t> memory_arena_;
    uintptr_t arena_next_ptr_; // Bump allocator pointer

    // List of static regions to also save.
    std::vector<MemoryRegion> static_regions_;

    // Pointers to original allocation functions.
    // (e.g., decltype(&malloc) original_malloc;)
};

// Hooked allocation function example
void* Hooked_Malloc(size_t size);
```

---

## 3. Next Steps & Action Plan

1.  **Create `GameStateManager` Stub**: Implement the basic class structure and memory arena.
2.  **Identify and Hook Allocators**: Research FM2K's binary to find all functions that allocate memory. This is the most critical and research-intensive step. We may need to hook `HeapAlloc`, `new`, `malloc`, and potentially custom object pool allocators.
3.  **Implement `SaveState`/`LoadState`**: Write the logic to copy the static regions and the memory arena into/out of the GekkoNet buffers.
4.  **Refactor Game Loop**: Modify the main loop to use the `gekko_update_session` model.
5.  **Incremental Testing**: Test each component in isolation:
    *   Does the game run correctly with the hooked allocator?
    *   Can we save and load a state offline and have the game continue identically?
    *   Does the `AdvanceFrame` event work as expected? 