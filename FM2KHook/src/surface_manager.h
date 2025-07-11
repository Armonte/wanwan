#pragma once
#include "sdl3_types.h"

// Forward declarations
struct SDL3Surface;

// Surface management functions
bool CreateSDL3Surfaces();
SDL3Surface* GetPrimarySurface();
SDL3Surface* GetBackSurface(); 
SDL3Surface* GetSpriteSurface();