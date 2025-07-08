#pragma once

// ... existing includes ...
#include "FM2KHook/src/ipc.h"  // For IPC::Event

class FM2KGameInstance {
public:
    // ... existing declarations ...

private:
    // IPC event handling
    void ProcessIPCEvents();
    void OnFrameAdvanced(const FM2K::IPC::Event& event);
    void OnStateSaved(const FM2K::IPC::Event& event);
    void OnStateLoaded(const FM2K::IPC::Event& event);
    void OnHookError(const FM2K::IPC::Event& event);

    // Shared memory view of event buffer
    FM2K::IPC::EventBuffer* event_buffer_;
    
    // ... existing members ...
}; 