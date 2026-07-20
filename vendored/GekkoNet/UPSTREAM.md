GekkoNet -- vendored in-tree (flattened from a submodule on 2026-06-17)
=======================================================================

Upstream:  https://github.com/HeatXD/GekkoNet
Base commit (last upstream commit we vendored from):
    b5c6528  Add self disconnect to the online example
             (tag v20260719222133; updated from 8ca4058 on 2026-07-20)

Why this is flattened (committed as plain files) instead of a git submodule
---------------------------------------------------------------------------
This tree carries a handful of local patches that exist on NO public remote --
a fresh `git submodule update` could never fetch them, so the old submodule pin
made the repo un-cloneable for anyone but the original author's machine.
Flattening the source in-tree guarantees the repo builds from a plain
`git clone`.

RECOMMENDED next step (kills the manual-merge pain): fork HeatXD/GekkoNet, put
the patches below on a `fm2k` branch of the fork, and re-add vendored/GekkoNet
as a submodule of THAT fork's branch. Then "update" becomes
`git fetch upstream && git rebase upstream/main` (git does the 3-way merge) and
the repo stays cloneable because the fork is public. See the 2026-07-20 update
note for how the b5c6528 merge was already done git-natively.

Local patches on top of upstream (what they do)
-----------------------------------------------
  1. gekko_confirmed_frame(): highest frame with REAL inputs from all players
     (never predicted) -- lets the host gate replay/spectator recording on
     confirmed-only inputs. (public API in gekkonet.h + gekkonet.cpp;
     ConfirmedFrame() = _sync.GetMinReceivedFrame() in each session.)
  2. Late spectator-join support: configurable GekkoConfig::input_history_size
     (Init gains a history_size arg; _max_input_queue_size replaces the fixed
     128 cap) + non-gating spectator handshake (is_spectator_pass in backend).
  3. gekko_wire_stats() / GekkoWireStat (#56 forensics): per-process wire-stage
     counters (g_gekko_wire_stats[8]) so a one-directional input-admit wedge
     names the stage that dropped the flow. Includes the #56 ROOT FIX in
     OnInputAck: reject acks for frames we never sent (stale cross-session ack
     used to freeze SendInputsToPeer). See project_battle_entry_wedge memory.
  4. backend: silently drop magic-mismatched packets during the sync handshake
     (replaces upstream's printf("dropped packet!") spam), counted as
     g_gekko_wire_stats[0].

Total local delta = 8 files, ~+160/-12 lines (backend.cpp, game_session.cpp,
gekkonet.cpp, spectator_session.cpp, stress_session.cpp, backend.h, gekkonet.h,
session.h). It is small and surgical -- it applies onto a reformatted/rewritten
upstream cleanly under a whitespace-insensitive 3-way merge (see below).

How the 2026-07-20 update (8ca4058 -> b5c6528) was done -- reuse this recipe
----------------------------------------------------------------------------
Do NOT hand-apply patches onto rewritten files. Let git's 3-way merge do it:

  1. Clone upstream to a scratch repo; fetch the target tip by SHA.
  2. Reconstruct our local delta as ONE commit on the OLD base:
       git checkout <old_base> -b fm2k-base
       git checkout <old_base> -- GekkoLib          # clean working tree
       cp <the 8 patched files> from vendored/GekkoNet/GekkoLib over it
       git add -A GekkoLib && git commit             # == our patches, git-native
     (Copy ONLY the 8 genuinely-changed files -- copying all of GekkoLib drags
      in phantom index/eol-normalization diffs. `diff -rq` the two trees first.)
  3. Cherry-pick onto the new tip, whitespace-insensitively so the upstream
     tabs->spaces reformat auto-resolves and only REAL conflicts surface:
       git checkout <new_tip> -b fm2k-final
       git cherry-pick -Xignore-all-space fm2k-base
     For b5c6528 this gave exactly ONE conflict, in OnInputs -- the serializer
     upgrade (#51) changed pkt.body from shared_ptr(.get()) to std::variant
     (std::get_if). Keep upstream's accessor + null-check, put our line after.
  4. AUDIT before trusting a clean merge: `git diff <new_tip>..fm2k-final --
     GekkoLib/src/backend.cpp` must show ONLY our additions -- no reverted
     upstream serializer code, no lingering `.get()` body access.
  5. Copy GekkoLib/{src,include,thirdparty} back over this tree. Only GekkoLib/
     is compiled (root CMakeLists.txt add_library(GekkoNet STATIC ...)).

Build integration notes (things upstream moved that our CMake must track)
-------------------------------------------------------------------------
  - b5c6528 moved internal headers to include/private/ (src still does
    #include "backend.h"), so the root CMakeLists.txt GekkoNet target adds
    vendored/GekkoNet/GekkoLib/include/private to its include dirs.
  - thirdparty/{zpp,asio} include paths were already present; the Asio upgrade
    (#51) only added files under thirdparty/asio (same paths).
  - Our FM2KHook only ever #includes the public gekkonet.h, which is
    self-contained -- it does not need include/private.

License: see ./LICENSE (unchanged from upstream).
