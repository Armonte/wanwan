#ifndef SAVESTATE_H
#define SAVESTATE_H

#include "shared_mem.h"

// Function declarations for savestate handling logic
bool SaveCompleteGameState(SaveStateData* save_data, uint32_t frame_number);
bool LoadCompleteGameState(const SaveStateData* save_data);
void ProcessManualSaveLoadRequests();

#endif // SAVESTATE_H 