#pragma once

#include "common.h"

bool InitializeGekkoNet();
void CleanupGekkoNet();
bool AllPlayersValid();
void ConfigureNetworkMode(bool online_mode, bool host_mode);

// Network connection info for CSS TCP sync
uint16_t GetGekkoLocalPort();
const char* GetGekkoRemoteIP(); 