# FM2K Rollback Netcode ? Implementation Plan

> **Status:** Draft

---

## 1. Vision & Objectives

* Replace the legacy LilithPort solution with a modern, maintainable rollback netcode stack tailored for **Fighter Maker 2nd** (FM2K).
* Preserve original gameplay feel while enabling low-latency online play and deterministic re-simulation.
* Keep the codebase modular so that future engine upgrades (e.g., SDL 3 migration) integrate seamlessly.

---

## 2. High-Level Architecture

| Layer | Responsibility | Key Technologies |
|-------|----------------|------------------|
| **Launcher / Wrapper** | Boots FM2K with required DLLs, manages config, spawns log window | C++, CMake |
| **Memory Helper Core** | Safe read/write wrappers around FM2K process memory, address validation, logging | MinHook, custom helpers |
| **Hooking Layer** | Installs hooks (functions & vtable), intercepts game loop, input, hit detection | MinHook |
| **Rollback Engine** | Maintains input/history buffers, runs prediction, triggers re-simulation | GekkoNet, ring buffers |
| **Networking (P2P)** | UDP transport, state sync, endpoint NAT traversal, peer ping | GekkoNet |
| **Diagnostics & Tools** | In-game HUD, desync detector, replay dumper | Dear ImGui, JSON logs |

---

## 3. Module Breakdown

1. **Process-Memory Helpers**
   * Centralised wrappers ? `Read<T>(addr)`, `Write<T>(addr, value)`.
   * Validation vs current FM2K binary (checksum).
2. **Hook Registry**
   * Declarative list of all FM2K hooks (symbols + signature).
   * Automated install/uninstall & version check.
3. **Frame Step Interceptor**
   * Splits render (60 FPS) vs logic (100 FPS) concerns.
   * Provides deterministic entry point for rollback engine.
4. **GekkoNet Integration**
   * Bridge layer (`gekko_bridge.cpp`) owning `Gekko::Session`.
   * C-style hook callbacks (`Bridge_AddInput`, `Bridge_Update`).
   * State serialization handlers for save/load/checksum.
5. **Networking Layer**
   * P2P session establishment, ping measurement.
   * Reliable/ordered input exchange over UDP.
6. **Tooling & UX**
   * Hot-reloadable config (`json`).
   * In-game overlay (frame advantage, ping, rollback count).
   * Debug console with command dispatch.

---

## 4. Development Phases & Milestones

### Phase 0: Prerequisites & Infrastructure

**Critical Blockers:**

1. **MinHook DLL Integration**
   * Create `FM2KHook.dll` containing hook logic
   * Move `FM2K::Hooks::Init/Shutdown` into DLL
   * Implement DllMain with proper init/shutdown
   * Export necessary entry points

2. **Inter-Process Communication**
   * Choose between:
     a) Shared memory + ring buffer for events
     b) Named pipe event system
   * Implement basic "frame advanced" marker

3. **32-bit Build Pipeline**
   * CMake preset for win32-release
   * Verify MinHook x86 compatibility
   * CI/build documentation

**Strong Helpers:**

4. **State Management**
   * Expand state struct with:
     * Hit-judge tables (0x42470C-0x430120, ~1KB)
     * Round-system globals (0x470040-0x47006C)
     * Object pool buffer (390KB)
   * Optimize bulk memory operations

5. **Version Compatibility**
   * Create `OffsetTable` struct
   * PE checksum lookup system
   * Version-agnostic code structure

6. **Launch Sequence**
   * CreateProcessA (suspended)
   * DLL injection + IPC handshake
   * Process monitoring
   * Error handling & cleanup

7. **Safety & Utilities**
   * `ScopedHandle` RAII wrapper
   * `CHECK_WIN` macro system
   * Logging infrastructure

### Phases 1-8: Core Implementation

| Phase | Goal | Deliverables |
|-------|------|--------------|
| **0. Prep** | Verify FM2K binary versions & symbols | Binary checksum list, baseline test harness |
| **1. Infrastructure** | Build DLL, integrate MinHook, process-memory helpers | `fm2k_hook.dll`, unit tests |
| **2. Static Hook Mapping** | Manually port essential LilithPort hooks (input, game loop) | Hook registry, smoke tests |
| **3. Dynamic Scanning** | Replace hard-coded addresses with pattern scans | Signature DB, version-agnostic loader |
| **4. FrameStep Extraction** | Clean separation of render vs logic, stub rollback calls | `framestep.cpp` integration |
| **5. Rollback Core (Offline)** | Implement state save/load & local prediction | Deterministic re-sim tests |
| **6. Networking MVP** | P2P handshake, input sync, 2-player local LAN demo | `net_session.cpp` |
| **7. Full Online Rollback** | Re-simulation, input delay calc, desync recovery | Playtest build |
| **8. Polishing & QA** | HUD, config UI, error handling, documentation | ImGui overlay, release notes |

---

## 5. GekkoNet Integration Details

### Session Interface (`GekkoSession`)

Core entry points from hook layer:
```cpp
void Init(GekkoConfig* config);              // Configure FPS, input size, etc
void SetNetAdapter(GekkoNetAdapter*);        // UDP transport backend
int AddActor(GekkoPlayerType, GekkoNetAddr*);// Register players/spectators
void AddLocalInput(int player, void* input); // Push frame input
GekkoGameEvent** UpdateSession(int* count);  // Get next actions
float FramesAhead();                         // Dynamic input delay
```

### Integration Flow

1. **Initialization**
   * Create singleton `Gekko::Session` after hooks installed
   * Configure FPS (100), input size, rollback window

2. **Per-Frame Loop**
   * Collect FM2K input Å® `AddLocalInput`
   * Process `UpdateSession` events:
     * SAVE Å® Copy FM2K state to buffer
     * LOAD + ROLLBACK Å® Restore state, replay
     * ADVANCE Å® Run predicted/confirmed frame
   * Update input delay based on `FramesAhead()`

3. **State Management**
   * Define serialization callbacks:
     * `SaveState` Å® Copy critical memory regions
     * `LoadState` Å® Restore from buffer
     * `CalcChecksum` Å® Verify determinism

4. **Diagnostics**
   * Surface `PlayerStatus`, ping, advantage in HUD
   * Monitor rollback frequency and frame drops
   * Log state mismatches and desyncs

---

## 6. Non-Goals (Out of Scope for v1)

* Spectator/stream sync.
* Cross-version matchmaking.
* Re-architecting FM2K render pipeline beyond SDL 3 migration.
* Automated ranked matchmaking & lobbies (can be added later).

---

## 7. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Address drift between FM2K releases | Pattern scanning + checksum gating |
| Non-deterministic engine behaviour | Input/state hashing, fuzz tests |
| Large state size Å® perf hit | Selective snapshot + per-subsystem serializers |
| MinHook conflicts with anti-cheat/a-v | Code-signing, user documentation |

---

## 8. Open Questions / TBD

* Precise memory regions required for a minimal deterministic snapshot?
* Preferred network backend (reuse GekkoNet vs bespoke)?
* Frame pacing strategy after SDL 3 migration (use host vs fixed v-sync)?
* Compression of state packets for remote rollback debugging?

---

## 9. References & Further Reading

* LilithPort original source dump (annotated)
* [GGPO Networking 101](https://drive.google.com/file/d/0B85cPX7YQsZ5Z2I5dXFAX3J6NTg/view)
* SDL 3 Migration Strategy (`docs/LilithPort_SDL3_Migration_Strategy.md`)
* FRAMESTEP analysis (`docs/FRAMESTEP_ANALYSIS.md`)

---

*Last updated: <!-- YYYY-MM-DD -->* 