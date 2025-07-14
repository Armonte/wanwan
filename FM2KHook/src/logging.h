#pragma once

#include "common.h"

// File logging system
void InitializeFileLogging();
void CleanupFileLogging();
void CustomLogOutput(void* userdata, int category, SDL_LogPriority priority, const char* message);

// Input recording system
void InitializeInputRecording();
void CleanupInputRecording();
void RecordInput(uint32_t frame, uint32_t p1_input, uint32_t p2_input);

// Desync reporting
void GenerateDesyncReport(uint32_t desync_frame, uint32_t local_checksum, uint32_t remote_checksum);
void LogMinimalGameStateDesync(uint32_t desync_frame, uint32_t local_checksum, uint32_t remote_checksum); 