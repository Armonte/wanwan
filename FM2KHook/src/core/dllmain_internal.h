#pragma once
// dllmain.cpp <-> dllmain_logging.cpp shared surface. The async-logging + crash-
// handler section was split out of dllmain.cpp; DllMain (core) calls these three
// entry points during attach/detach. Fm2k_BuildLogPath is declared in globals.h;
// the quill logger state + LogOutputFunction/CrashHandler/VectoredRenderGuard
// stay file-local to dllmain_logging.cpp.
void InitFileLogging();
void InstallCrashHandler();
// Per-thread stack reserve for the crash handler; call on the game's main thread.
void Crash_ArmMainThread();
void ShutdownFileLogging();
