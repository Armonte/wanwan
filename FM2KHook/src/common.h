#pragma once

#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <memory>
#include <chrono>
#include <fstream>
#include <mutex>
#include <SDL3/SDL.h>

// Forward declarations
namespace FM2K {
    namespace State {
        struct GameState;
        struct CoreGameState;
    }
    struct MinimalGameState;
}

class GekkoSession;
struct GekkoGameEvent;
struct GekkoSessionEvent;
struct GekkoNetworkStats;
struct GekkoNetAddress; 