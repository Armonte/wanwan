#pragma once

#include "common.h"
#include <vector>
#include <string>
#include <windows.h>

bool InitializeGekkoNet();
void CleanupGekkoNet();
void GekkoNetUpdate();
void ProcessGekkoNetFrame();

// Complete main loop replacement with GekkoNet integration
BOOL GekkoNet_MainLoop();

// Network connection info for CSS TCP sync
uint16_t GetGekkoLocalPort();
const char* GetGekkoRemoteIP(); 