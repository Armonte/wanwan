#include "ipc.h"
#include <SDL3/SDL.h>

namespace FM2K {
namespace IPC {

// Constants
constexpr size_t MAX_EVENTS = 1024;
constexpr size_t BUFFER_SIZE = sizeof(Event) * MAX_EVENTS;

// Shared memory
static HANDLE shared_memory = nullptr;
static Event* event_buffer = nullptr;
static size_t event_count = 0;

bool Init() {
    // Create shared memory
    shared_memory = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, BUFFER_SIZE, L"FM2K_IPC_Buffer");
    
    if (!shared_memory) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to create shared memory: %lu", GetLastError());
        return false;
    }

    // Map shared memory
    event_buffer = static_cast<Event*>(MapViewOfFile(shared_memory,
        FILE_MAP_ALL_ACCESS, 0, 0, BUFFER_SIZE));
    
    if (!event_buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to map shared memory: %lu", GetLastError());
        CloseHandle(shared_memory);
        shared_memory = nullptr;
        return false;
    }

    // Initialize buffer
    memset(event_buffer, 0, BUFFER_SIZE);
    event_count = 0;

    return true;
}

void Shutdown() {
    if (event_buffer) {
        UnmapViewOfFile(event_buffer);
        event_buffer = nullptr;
    }

    if (shared_memory) {
        CloseHandle(shared_memory);
        shared_memory = nullptr;
    }

    event_count = 0;
}

bool PostEvent(const Event& event) {
    if (!event_buffer || event_count >= MAX_EVENTS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "IPC buffer full or not initialized");
        return false;
    }

    // Add event to buffer
    event_buffer[event_count++] = event;
    return true;
}

bool ReadEvent(Event& event) {
    if (!event_buffer || event_count == 0) {
        return false;
    }

    // Get next event
    event = event_buffer[0];

    // Shift remaining events
    --event_count;
    if (event_count > 0) {
        memmove(&event_buffer[0], &event_buffer[1],
            event_count * sizeof(Event));
    }

    return true;
}

bool PollEvent(Event* event) {
    if (!event || !event_buffer || event_count == 0) {
        return false;
    }

    // Get next event
    *event = event_buffer[0];

    // Shift remaining events
    --event_count;
    if (event_count > 0) {
        memmove(&event_buffer[0], &event_buffer[1],
            event_count * sizeof(Event));
    }

    return true;
}

bool WriteEvent(const Event& event) {
    if (!event_buffer || event_count >= MAX_EVENTS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "IPC buffer full or not initialized");
        return false;
    }

    // Add event to buffer
    event_buffer[event_count++] = event;
    return true;
}

} // namespace IPC
} // namespace FM2K 