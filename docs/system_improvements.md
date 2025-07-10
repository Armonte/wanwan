# FM2K Rollback System Improvements

## Introduction
This document consolidates our current understanding of FM2KÅfs hook layer, the GekkoNet rollback API, and lessons learned from the **Giuroll** project (EFZ).  The goal is to design a robust rollback architecture that cleanly separates **frame execution**, **state management**, and **network synchronisation** while preserving game stability.

---

## What We Know
1. **Hook Layer (FM2K)**
   - Critical execution points are already mapped and wrapped (`PROCESS_INPUTS_ADDR`, `UPDATE_GAME_STATE_ADDR`, `GAME_RAND_ADDR`).
   - A minimal frame counter and hook installation routine exist (see `FM2K_Hook_Implementation.md`).
   - Heap allocations occur through the gameÅfs custom allocator; addresses are stable across sessions.

2. **GekkoNet API** (`gekkonet.h`)
   - Provides a session object responsible for input collection and session events.
   - Exposes **game events** (`Advance`, `Save`, `Load`) and **session events** (sync, desync, etc.).
   - Operates on opaque input & state blobs supplied by the game.

3. **Giuroll Strategy (EFZ)**
   - Captures a **minimal** but deterministic snapshot every frame (registers, important globals, heap diffs).
   - Tracks heap allocations per-frame, undoing allocations when rolling back.
   - Confirms or corrects predicted inputs, triggering re-simulations on mismatch.

---

## Proposed System Improvements
### 1  Deterministic State Snapshots
*Implement a two-tier snapshot model:*  
`FullFrameState` (key frames, e.g., every 16th) + `DeltaFrameState` (intervening frames).

```cpp
struct FullFrameState {
    uint32_t frame_number;
    std::vector<uint8_t> registers;   // FPU + CPU context
    std::vector<uint8_t> heap_pages;  // raw memory dump of critical pages
    std::vector<AllocRecord> allocs;  // per-frame heap diffs
};

struct DeltaFrameState {
    uint32_t frame_number;
    std::vector<AllocRecord> allocs;  // only allocations/frees since last snapshot
};
```

`AllocRecord` mirrors GiurollÅfs design (ptr, size, is_free).

### 2  Heap Instrumentation Layer
1. Hook FM2KÅfs allocator entry points (detected via static analysis) and log every *alloc/free/realloc* with the current `frame_counter_`.
2. Maintain a **frame-indexed heap log** enabling *undo* operations during rollback.
3. Guard allocations with canaries to detect overruns (optional but recommended).

#### Critical Memory Regions (harvested from `rewindstates.txt`)
Following GiurollÅfs approach we must actively walk the *battle* singleton and its nested vectors / linked-lists each frame:

- `0x8985F4` Battle root Å® captures players, projectiles, effects.
- `0x8985E8` Weather / camera system lists (variable length `read_linked_list`).
- `0x8986A0` Netplay input buffers (two ring-buffers per player) ? required for re-sim.
- Character-specific bullet chains resolved via `CHARSIZEDATA[chr].1`, then pointer-chased through `get_ptr()`.

These addresses are gathered using `read_addr()`, `read_vec()`, and `read_linked_list()` helpers to build a *flat* byte-blob appended to the current frame snapshot.  Because FM2KÅfs heap is shared, we only dump **pointer + size** metadata; the allocator hook provides the actual heap pages.

### 3  GekkoNet Integration Workflow
| Frame Phase            | FM2K Responsibility                        | GekkoNet Call               |
|------------------------|---------------------------------------------|-----------------------------|
| Collect local input    | Read PAD memory Å® `gekko_add_local_input()` | ?                           |
| Begin frame advance    | ?                                           | `gekko_update_session()`    |
| Apply confirmed inputs | Overwrite PAD memory                        | ?                           |
| Process game logic     | `PROCESS_INPUTS_ADDR` Å® `UPDATE_GAME_STATE_ADDR` | ?                     |
| Snapshot / Save event  | If `ShouldSaveState()` Å® build state blob   | Emit `SaveEvent`            |
| Load event             | On `LoadEvent` Å® restore snapshot & heap    | ?                           |

`state blob` == serialised `FullFrameState` **or** `DeltaFrameState`.

### 4  Desync Detection
Borrow GiurollÅfs lightweight checksum model:
```cpp
uint32_t CalculateChecksum() {
    // FM2K example: xor of important timers + RNG seed + effect flags
    return *(uint32_t*)0x447EE0 ^ *(uint32_t*)0x40CC30 ^ current_rand_seed;
}
```
Send checksum every *n* frames via `AdvanceEvent`.  Compare in `DesyncDetected` session event.

### 5  Sound & Visual Consistency
*Sounds*: queue speculative sounds; cancel those not replayed after rollback.  
*Visuals*: leverage `VisualStateChanged()` to mark frames where VFX buffers diverge, prompting a `FullFrameState` save.

### 6  Debug Telemetry & Stress Harness
Implement an optional SDL3 console overlay (or log-to-file build flag) that reports:
- `HEAP_OP_LOGGED` events (from allocator hooks)
- Frame #, rollback count, current delay/max rollback
- Memory snapshot size per frame and running total

---

## Implementation Roadmap (How-Focused)
1. **Allocator Hooks** ? Identify allocator functions, inject pre/post logging.
2. **Snapshot Builder** ? Serialize register + critical global regions (see table in research doc).
3. **Delta Compression** ? XOR diff heap pages; store only non-zero bytes.
4. **GekkoNet Glue** ? Implement *Save*, *Load*, *Advance* callbacks.
5. **Checksum Channel** ? Embed checksum in `AdvanceEvent` payload.
6. **Stress Harness** ? Deterministic replay script driving FM2K headless for 10k frames.

---

## Discovery Plan
The following open questions require targeted investigation.  Each task references our IDA-Pro MCP toolkit (xref, decompile, etc.) and the `FM2K_Rollback_Research.md` data set.

| ID | Focus Area | Research Actions | Success Criteria |
|----|------------|------------------|------------------|
| D-1 | Complete **Critical Globals** list | 1. Use `mcp_get_entry_points` + `mcp_list_globals_filter \"dword_\"` to enumerate RW globals.<br/>2. Cross-reference (`mcp_get_xrefs_to`) each global touched inside: `physics_collision_system`, `hit_detection_system`, `character_input_processor`, `ai_input_processor`.<br/>3. Add any state-mutating globals to `FM2K_Rollback_Research.md` memory map. | All write-sites to unknown globals are catalogued and their role classified (logic, visual, config). |
| D-2 | RNG behaviour under allocator hooks | 1. Set breakpoint on `game_rand` (0x417A22) and log seed reads/writes.<br/>2. Enable allocator hooks; record seed before/after heavy allocation bursts.<br/>3. Compare with control run (hooks disabled). | RNG seed remains deterministic (no corruption) or deviation pattern identified & mitigated. |
| D-3 | **Snapshot Frequency** benchmarking | 1. Instrument `FullFrameState` save routine with high-res timers (SDL_GetPerformanceCounter).<br/>2. Run 1-, 2-, 4-, 8-, 16-frame key-frame intervals with 60-frame deltas; capture average & p99 save/restore times.<br/>3. Record mem-copy volume per frame. | Determine max key-frame interval that keeps <1 ms overhead and <64 MB total buffer. |
| D-4 | Feasibility of `post_sync_joining` | 1. Simulate late-join scenario: create session, advance 300 frames, then add Spectator via `gekko_add_actor(RemotePlayer)`.<br/>2. Measure time GekkoNet needs to sync state (size of `state_size` vs. network throughput).<br/>3. Stress-test with 400 KB state payload. | Joining player can reach sync <2 s on 10 Mbps link without desync.  |

Ownership: `@thorns` leads D-1 & D-2; `@montobot` leads D-3; `@jamie` prototypes D-4.

Progress will be logged in `docs/ROLLBACK_INSIGHTS.md`.

---

## References
- `FM2K_Hook_Implementation.md`
- `gekkonet.h`
- Giuroll source analysis (EFZ rollback netcode) 