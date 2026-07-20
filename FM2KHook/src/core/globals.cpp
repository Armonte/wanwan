#include "globals.h"

// Original function pointers (set by MinHook)
GetPlayerInputFunc original_get_player_input = nullptr;
GetPlayerInputFM95Func original_get_player_input_p1 = nullptr;  // FM95 only
GetPlayerInputFM95Func original_get_player_input_p2 = nullptr;  // FM95 only
UpdateGameStateFunc original_update_game = nullptr;
RunGameLoopFunc original_run_game_loop = nullptr;
RenderGameFunc original_render_game = nullptr;
GameRandFunc original_game_rand = nullptr;
ProcessGameInputsFunc original_process_game_inputs = nullptr;
BlitSpriteFunc original_blit_sprite = nullptr;  // set only under FM2K_RENDER_PROFILE / FM2K_BLIT_SIMD
SpriteRenderEngineFunc original_sprite_render_engine = nullptr;  // set only under FM2K_BLIT_SIMD

// Render sub-profiler accumulators (see globals.h). Cumulative; display-only.
volatile uint32_t g_rp_blit_calls = 0;
volatile uint64_t g_rp_blit_ns = 0;
volatile uint64_t g_rp_blit_area = 0;
volatile uint32_t g_rp_blit_mode[5] = {0, 0, 0, 0, 0};

// Render RNG stream (see globals.h). Re-seeded from the gameplay seed each
// render; advanced only by render-side game_rand draws via Hook_GameRand.
uint32_t g_render_rng_seed = 0;
bool     g_in_render_rng   = false;
// Diagnostic counter (#63): render-side game_rand calls, to test whether the
// Robot Heroes heavy-stage render cost is our Hook_GameRand overhead scaling
// with per-frame rng draws. Reset per offline frame by the trampoline.
uint32_t g_render_rand_calls = 0;
// Mid-join spectate desync hunt: count GAMEPLAY-seed game_rand draws (calls
// with g_in_render_rng == false). If the spectator's per-frame gameplay-rand
// count diverges from the host's, that frame has an extra/missing gameplay
// draw = the leak. Logged + reset per parity Capture() via [FULLFP].
uint32_t g_gameplay_rand_calls = 0;
// Per-caller gameplay-rand tally (spectate desync hunt). Index: 0=camera_manager
// 1=ProcessShakeEffect 2=ProcessColorInterpolation 3=sprite_rendering_engine
// 4=hit_detection_system 5=ai_input_processor 6=character_state_machine 7=other.
// Reset + logged per parity Capture() via [FULLFP] fn=. Whichever caller's
// per-frame count first diverges host-vs-spectator names the leaking function.
uint32_t g_gp_rand_by_fn[8] = {0};
volatile uint32_t g_sim_step_count = 0;

// Minimal global state
int g_player_index = 0;
uint32_t g_frame_counter = 0;
bool g_is_rolling_back = false;
bool g_css_rollback = false;   // #66: CSS rollback opt-in (FM2K_CSS_ROLLBACK=1)

// Network config (parsed at startup, used when entering battle)
bool g_offline_mode = false;
uint16_t g_local_port = 7000;
char g_remote_addr[64] = "127.0.0.1:7001";

// Stress-test mode (FM2K_STRESS_MODE=1): single-instance determinism test
bool g_stress_mode = false;

// Spectator mode (FM2K_SPECTATOR_MODE=1): passive viewer.
bool g_spectator_mode = false;

// Spectator fast catch-up flag (C5.5). Set by RunSpectatorTick's inner
// loop while burning through queued events; cleared once pb_queue depth
// drops below LIVE_LAG_FRAMES.
bool g_spectator_catchup = false;

// User-toggled FF (F12 in spectator window). Persistent across
// catchup-loop iterations; flipped by WndProc subclass on key press.
bool g_spectator_ff_user = false;

// FM95 host-driven trampoline: when Hook_UpdateGameState runs the
// trampoline tick on the FM95 build, it sets this to tell the host's
// natural render_game call (caught by Hook_RenderGame) to skip its body
// — the trampoline already drove RenderFrameWithSnapshot inside the
// tick, calling original_render_game once. Without this, FM95 renders
// twice per frame: once via RenderFrameWithSnapshot, once via the
// host's natural render_game pump. Cleared at the top of Hook_RenderGame
// after the skip fires.
bool g_fm95_skip_next_render = false;
bool g_fm95_in_sim_tick = false;

// FM95 loop-ownership: set true once the inline patch at the WinMain loop head
// (0x40AD67) is installed, so TrampolineMainLoop owns the loop exactly like
// FM2K owns main_game_loop. When true, the host-driven Hook_UpdateGameState
// dispatch is bypassed and timeGetTime virtualizes like FM2K (no g_fm95_in_sim
// _tick gate -- there is no host WinMain pacing left to protect). If the patch
// fails to install this stays false and we fall back to the host-driven path.
bool g_fm95_loop_owned = false;
