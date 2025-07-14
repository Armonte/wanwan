#pragma once

#include "common.h"

bool InitializeGekkoNet();
void CleanupGekkoNet();
bool AllPlayersValid();
void ConfigureNetworkMode(bool online_mode, bool host_mode); 