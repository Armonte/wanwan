#include "ipc.h"
#include <SDL3/SDL.h>

namespace FM2K {
namespace IPC {

// Constants
constexpr size_t BUFFER_SIZE = sizeof(EventBuffer);

// Shared memory
static HANDLE g_shmem_handle = nullptr;
static EventBuffer* g_event_buffer = nullptr;

bool Init() {
    // Try to open existing shared memory first
    g_shmem_handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"FM2K_IPC_Buffer");
    bool is_new_buffer = false;
    
    if (!g_shmem_handle) {
        // Shared memory doesn't exist - create it
        g_shmem_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, BUFFER_SIZE, L"FM2K_IPC_Buffer");
        is_new_buffer = true;
        
        if (!g_shmem_handle) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                "Failed to create shared memory: %lu", GetLastError());
            return false;
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Created new IPC shared memory buffer");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "Connected to existing IPC shared memory buffer");
    }

    // Map shared memory
    g_event_buffer = static_cast<EventBuffer*>(MapViewOfFile(g_shmem_handle,
        FILE_MAP_ALL_ACCESS, 0, 0, BUFFER_SIZE));
    
    if (!g_event_buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to map shared memory: %lu", GetLastError());
        CloseHandle(g_shmem_handle);
        g_shmem_handle = nullptr;
        return false;
    }

    // Only initialize buffer if we created it
    if (is_new_buffer) {
        memset(g_event_buffer, 0, BUFFER_SIZE);
        g_event_buffer->read_index = 0;
        g_event_buffer->write_index = 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "IPC: CREATED new buffer at %p (write: %ld, read: %ld)", 
            g_event_buffer, g_event_buffer->write_index, g_event_buffer->read_index);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "IPC: CONNECTED to existing buffer at %p (write: %ld, read: %ld)", 
            g_event_buffer, g_event_buffer->write_index, g_event_buffer->read_index);
    }

    return true;
}

void Shutdown() {
    if (g_event_buffer) {
        UnmapViewOfFile(g_event_buffer);
        g_event_buffer = nullptr;
    }

    if (g_shmem_handle) {
        CloseHandle(g_shmem_handle);
        g_shmem_handle = nullptr;
    }
}

bool PostEvent(const Event& event) {
    if (!g_event_buffer) {
        return false;
    }
    
    if (g_event_buffer->IsFull()) {
        // This can get spammy, so it's commented out.
        // SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "IPC buffer full!");
        return false;
    }

    // Add event to ring buffer
    LONG write_index = g_event_buffer->write_index;
    g_event_buffer->events[write_index] = event;
    
    // Atomically update write index
    LONG new_write_index = (write_index + 1) % EventBuffer::BUFFER_SIZE;
    InterlockedExchange(&g_event_buffer->write_index, new_write_index);
    
    return true;
}

bool PollEvent(Event* event) {
    if (!g_event_buffer || g_event_buffer->IsEmpty()) {
        return false;
    }

    // Get next event from ring buffer
    LONG read_index = g_event_buffer->read_index;
    *event = g_event_buffer->events[read_index];

    // Atomically update read index
    LONG new_read_index = (read_index + 1) % EventBuffer::BUFFER_SIZE;
    InterlockedExchange(&g_event_buffer->read_index, new_read_index);

    return true;
}

bool IsInitialized() {
    return g_shmem_handle != nullptr && g_event_buffer != nullptr;
}

} // namespace IPC
} // namespace FM2K