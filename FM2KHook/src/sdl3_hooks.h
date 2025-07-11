#pragma once

#include <SDL3/SDL.h>
#include <windows.h>

// This structure will hold all our global SDL-related state.
struct SDL3Context {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* gameTexture;   // The texture representing the game's final output.
    int gameWidth;
    int gameHeight;
    int windowWidth;
    int windowHeight;
    bool initialized;
    bool isFullscreen;
};

// A global instance of our context.
extern SDL3Context g_sdlContext;

// --- Core SDL3 Management Functions ---

// Initializes the SDL3 video and event systems.
bool InitializeSDL3();

// Creates the main SDL3 window, renderer, and game texture.
// It takes the game's native HWND to dock our SDL window to it.
bool CreateSDL3Context(HWND hwnd);

// Cleans up all SDL resources.
void CleanupSDL3();

// --- Rendering Functions ---

// Renders the game's texture to the window with correct aspect ratio scaling.
void RenderGame();

// --- Event & Input Handling ---

// The main window procedure that will replace the one from SDL.
// It will forward messages to both SDL and the original game logic.
LRESULT CALLBACK InterceptedWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// This function will be called every frame to pump the SDL event loop.
void PollSDLEvents();

// Toggles between fullscreen and windowed mode.
void ToggleFullscreen();
void SetOriginalWindowProc(WNDPROC proc); 