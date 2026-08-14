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

!! THE b5c6528 BUMP ITSELF IS A WIRE BREAK (recorded 2026-08-13) !!
------------------------------------------------------------------
The 8ca4058 -> b5c6528 bump (our commit 0dde1f5) took upstream's serializer
upgrade (#51): thirdparty/zpp/serializer.h is DELETED and thirdparty/zpp_bits.h
added, and every archive call site in GekkoLib/src/backend.cpp is rewritten from
zpp::serializer::memory_{input,output}_archive to zpp::bits::{in,out}. That
swaps polymorphic registered types for a flat serializer, which changes the
BYTE FORMAT of every Gekko message -- an old peer cannot parse a new packet.

  * Builds at or after that bump are wire-INCOMPATIBLE with v0.2.83-bleeding
    and everything earlier. This is not a soft degradation; the handshake and
    input packets simply do not decode.
  * The protection is the hub's client_version equality gate, which only works
    if the version actually differs -- so FM2K_VERSION was bumped 0.2.83 ->
    0.2.84 the same day (scripts/make_version.sh). Do not ship a build carrying
    this bump under the old version string.
  * Direct-IP sessions between a pre-bump and a post-bump peer bypass the hub
    gate and WILL fail. That is expected behavior for a wire break, not a bug --
    the fix is "both sides update".

!! STOP -- READ BEFORE UPDATING PAST b5c6528 (assessed 2026-07-27) !!
--------------------------------------------------------------------
Upstream tip 675b31d "ReplaySession (#55)" (2026-07-26) is **WIRE-BREAKING**.
It is one squashed commit over our base: 22 files, +1133/-327.

  * backend.cpp now DELTA-encodes inputs before RLE on send and DeltaDecodes
    after RLE on receive (stride = _input_size, or _input_size*_num_players for
    SpectatorInputs packets), and MAX_INPUT_SIZE goes 512 -> 1024. It is
    signalled by the SAME "compressed" boolean as before, so a b5c6528 peer
    receiving a 675b31d packet reverses only the RLE and gets GARBAGE INPUTS --
    silent corruption, NOT a clean version refusal. Adopting this must be a
    coordinated hard-break release enforced by our version gate, never a quiet
    bleeding bump.
  * private/session.h is DELETED (-240) and split into private/session/{session,
    game_session,spectator_session,stress_session,replay_session}.h. Our
    session.h patch has to be RELOCATED, not merged. 7 of our 8 patched files
    are touched (only backend.h is untouched). gekkonet.h also gains ~30 lines
    of doc comments = pure merge noise.
  * Value to us is LOW: their replay records confirmed inputs into a generic
    blob (gekko_start_recording / gekko_stop_recording / gekko_load_replay,
    GekkoReplaySession kind, GekkoReplayFinished event). We already have a
    richer FM2K-specific replay layer (.fm2krep/.fm2kset carrying round config,
    stage id, char ids, seek-to-match, whole-set playback) that a generic
    input-only replay cannot represent, and switching would break every
    existing user replay file. Their recorder does solve the same problem our
    local patch #1 (gekko_confirmed_frame) exists for, so IF we ever adopt it,
    patch #1 can retire.

GOOD NEWS -- THE WIRE BREAK IS ISOLATED AND SKIPPABLE. On main we are exactly
ONE squashed merge behind (b5c6528..675b31d = 675b31d alone), but the PR's
constituent commits are still reachable, and the wire change is its own commit:

    00ba527  Compress network inputs with delta+RLE and raise cap to 1024

Everything else in the merge is replay machinery (882e179 replay session +
public API, 1c616f2 recording, 71232c0 record in game/spectator/stress,
9ccf0b6 uncompressed option, 97a5f76 example, cd84338 README), the session
header split (8a44363), API doc comments (1063c28), and interface defaults
(a79c436). Audited 2026-07-27: there is NO other netcode fix or perf work
hiding in there -- the only non-replay change IS the wire break.

So a future update can take the merge and REVERT/skip 00ba527 to stay
wire-compatible with shipped clients, or take it deliberately as a versioned
hard break. Do the fork-and-submodule step below FIRST -- this update is
exactly the kind that makes hand-replay hurt.

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
