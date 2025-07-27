#pragma once

#include "common.h"
#include <vector>
#include <string>
#include <windows.h>

bool InitializeGekkoNet();
void CleanupGekkoNet();
void GekkoNetUpdate();
void ProcessGekkoNetFrame();
void ApplyNetworkedInputsImmediately(uint16_t p1_input, uint16_t p2_input);
void WriteNetworkedInputsToMemory(uint16_t p1_input, uint16_t p2_input);
// BSNES PATTERN: Immediate input application - no pending functions needed

// Functions for Hook_GetPlayerInput to access networked inputs
uint16_t GetCurrentNetworkedP1Input();
uint16_t GetCurrentNetworkedP2Input();

// Complete main loop replacement with GekkoNet integration
BOOL GekkoNet_MainLoop();

// Network connection info for CSS TCP sync
uint16_t GetGekkoLocalPort();
const char* GetGekkoRemoteIP();

// BSNES-style network health monitoring
extern uint32_t netplay_counter;           // Frame counter for network monitoring
extern uint32_t netplay_stall_counter;     // Stall detection counter
extern bool netplay_run_ahead_mode;        // Run-ahead mode flag for rollback
extern float netplay_local_delay;          // Local delay setting

// Network health functions
void MonitorNetworkHealth();
void HandleFrameDrift();
void FM2K_NetplayHaltFrame();  // FM2K equivalent of BSNES netplayHaltFrame() 