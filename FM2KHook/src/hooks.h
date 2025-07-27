#ifndef HOOKS_H
#define HOOKS_H

#include "globals.h"

// Hook function declarations
bool InitializeHooks();
void ShutdownHooks();
int __cdecl Hook_UpdateGameState();
void __cdecl Hook_RenderGame();
BOOL __cdecl Hook_RunGameLoop();
char __cdecl Hook_CSS_Handler();
int __cdecl Hook_GetPlayerInput(int player_index, int controller_index);

#endif // HOOKS_H 