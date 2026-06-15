#pragma once

// Install the "lazy DirectSound buffer" CSS optimization, gated on
// FM2K_FPK_CSS_FASTSOUND=1. Character sound DirectSound buffers are deferred at
// load and built on first play, so CSS cursor moves don't pay ~80 CreateSoundBuffer
// COM calls per hover. See css_fastsound.cpp for the full rationale.
//
// Queues its MinHook hooks; must be called while installing the other file/IO
// hooks, BEFORE the batched MH_ApplyQueued, and after MH_Initialize.
void CssFastSound_Install();
