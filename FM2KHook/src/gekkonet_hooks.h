#pragma once

#include "common.h"
#include <vector>
#include <string>

bool InitializeGekkoNet();
void CleanupGekkoNet();
void GekkoNetUpdate();
void ProcessGekkoNetFrame();

// Network connection info for CSS TCP sync
uint16_t GetGekkoLocalPort();
const char* GetGekkoRemoteIP(); 