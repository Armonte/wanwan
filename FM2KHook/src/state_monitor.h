#ifndef STATE_MONITOR_H
#define STATE_MONITOR_H

#include <cstdint>

void MonitorGameStateTransitions();
const char* GetGameModeString(uint32_t mode);

#endif // STATE_MONITOR_H
