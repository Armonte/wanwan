#include "ipc.h"
#include <SDL3/SDL.h>

namespace FM2K {
namespace IPC {

// Constants
constexpr size_t BUFFER_SIZE = sizeof(EventBuffer);

// Shared memory
static HANDLE shared_memory = nullptr;
static EventBuffer* event_buffer = nullptr;

bool Init() {
    // Try to open existing shared memory first
    shared_memory = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"FM2K_IPC_Buffer");
    bool is_new_buffer = false;
    
    if (!shared_memory) {
        // Shared memory doesn't exist - create it
        shared_memory = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, BUFFER_SIZE, L"FM2K_IPC_Buffer");
        is_new_buffer = true;
        
        if (!shared_memory) {
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
    event_buffer = static_cast<EventBuffer*>(MapViewOfFile(shared_memory,
        FILE_MAP_ALL_ACCESS, 0, 0, BUFFER_SIZE));
    
    if (!event_buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "Failed to map shared memory: %lu", GetLastError());
        CloseHandle(shared_memory);
        shared_memory = nullptr;
        return false;
    }

    // Only initialize buffer if we created it
    if (is_new_buffer) {
        memset(event_buffer, 0, BUFFER_SIZE);
        event_buffer->read_index = 0;
        event_buffer->write_index = 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "IPC: CREATED new buffer at %p (write: %ld, read: %ld)", 
            event_buffer, event_buffer->write_index, event_buffer->read_index);
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "IPC: CONNECTED to existing buffer at %p (write: %ld, read: %ld)", 
            event_buffer, event_buffer->write_index, event_buffer->read_index);
    }

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

}

bool PostEvent(const Event& event) {
    if (!event_buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "IPC buffer not initialized");
        return false;
    }
    
    if (event_buffer->IsFull()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
            "IPC buffer full (write: %ld, read: %ld)", 
            event_buffer->write_index, event_buffer->read_index);
        return false;
    }

    // Add event to ring buffer
    LONG write_index = event_buffer->write_index;
    event_buffer->events[write_index] = event;
    
    // Atomically update write index
    LONG new_write_index = (write_index + 1) % EventBuffer::BUFFER_SIZE;
    InterlockedExchange(&event_buffer->write_index, new_write_index);
    
    // Reduced logging - only log errors and occasional status
    static int post_count = 0;
    if (++post_count % 1000 == 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
            "Posted %d IPC events (latest: type %d, frame %u)", 
            post_count, (int)event.type, event.frame_number);
    }
    
    return true;
}

bool ReadEvent(Event& event) {
    if (!event_buffer || event_buffer->IsEmpty()) {
        return false;
    }

    // Get next event from ring buffer
    LONG read_index = event_buffer->read_index;
    event = event_buffer->events[read_index];

    // Atomically update read index
    LONG new_read_index = (read_index + 1) % EventBuffer::BUFFER_SIZE;
    InterlockedExchange(&event_buffer->read_index, new_read_index);

    return true;
}

bool PollEvent(Event* event) {
    if (!event) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PollEvent: null event pointer");
        return false;
    }
    
    if (!event_buffer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PollEvent: null event_buffer");
        return false;
    }
    
    if (event_buffer->IsEmpty()) {
        static int empty_count = 0;
        // Remove empty buffer spam
        return false;
    }

    // Get next event from ring buffer
    LONG read_index = event_buffer->read_index;
    *event = event_buffer->events[read_index];

    // Atomically update read index
    LONG new_read_index = (read_index + 1) % EventBuffer::BUFFER_SIZE;
    InterlockedExchange(&event_buffer->read_index, new_read_index);

    // Reduced logging
    static int poll_count = 0;
    if (++poll_count % 1000 == 0) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
            "Polled %d IPC events (latest: type %d, frame %u)", 
            poll_count, (int)event->type, event->frame_number);
    }

    return true;
}

bool WriteEvent(const Event& event) {
    return PostEvent(event);
}

} // namespace IPC
} // namespace FM2K 