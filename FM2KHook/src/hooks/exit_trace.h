#pragma once
// [EXIT] trace: hooks kernel32!ExitProcess so every graceful process exit
// logs WHO asked for it (module+offset call stack, thread, exit code, game
// mode) before the process goes. A game that "just closed" with no WM_QUIT and
// no crash-handler output (crash vs Ricky, 2026-11-08) left nothing to go on;
// this turns that class into a one-line answer. Always on: it costs nothing
// until the process is exiting.

// Queues the hook; applied by the MH_ApplyQueued batch in InitializeHooks.
void ExitTrace_Install();

// Records the game's main thread (Install runs on the injector's thread), so
// the [EXIT] line can say whether the main loop or some other thread exited.
void ExitTrace_SetMainThread(unsigned long tid);
