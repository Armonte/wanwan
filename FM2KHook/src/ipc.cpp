#include "ipc.h"
#include <atomic>

namespace FM2K {
namespace IPC {

// Shared memory handles
static HANDLE mapping_handle = nullptr;
static EventBuffer* buffer = nullptr;

// Name of the shared memory mapping (must match in launcher)
static const wchar_t* MAPPING_NAME = L"Local\\FM2K_EventBuffer";

bool Init() {
    // Create/open the shared memory mapping
    mapping_handle = CreateFileMappingW(
        INVALID_HANDLE_VALUE,    // Use paging file
        nullptr,                 // Default security
        PAGE_READWRITE,         // Read/write access
        0,                      // Max size (high)
        sizeof(EventBuffer),    // Max size (low)
        MAPPING_NAME           // Name
    );
    
    if (!mapping_handle) {
        return false;
    }
    
    // Map the shared memory into our address space
    buffer = static_cast<EventBuffer*>(
        MapViewOfFile(
            mapping_handle,
            FILE_MAP_ALL_ACCESS,
            0, 0,                // Offset
            sizeof(EventBuffer)  // Size
        )
    );
    
    if (!buffer) {
        CloseHandle(mapping_handle);
        mapping_handle = nullptr;
        return false;
    }
    
    // Initialize indices if we created the mapping
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        buffer->read_index = 0;
        buffer->write_index = 0;
    }
    
    return true;
}

void Shutdown() {
    if (buffer) {
        UnmapViewOfFile(buffer);
        buffer = nullptr;
    }
    if (mapping_handle) {
        CloseHandle(mapping_handle);
        mapping_handle = nullptr;
    }
}

bool PostEvent(EventType type, uint8_t player) {
    if (!buffer) return false;
    
    // Check if buffer is full
    if (buffer->IsFull()) return false;
    
    // Write the event
    Event& ev = buffer->events[buffer->write_index];
    ev.type = type;
    ev.player_index = player;
    ev.frame_number = 0; // TODO: Get from game state
    ev.timestamp_ms = GetTickCount();
    
    // Advance write index
    InterlockedIncrement(&buffer->write_index);
    buffer->write_index %= EventBuffer::BUFFER_SIZE;
    
    return true;
}

bool TryReadEvent(Event* out_event) {
    if (!buffer || !out_event) return false;
    
    // Check if buffer is empty
    if (buffer->IsEmpty()) return false;
    
    // Read the event
    *out_event = buffer->events[buffer->read_index];
    
    // Advance read index
    InterlockedIncrement(&buffer->read_index);
    buffer->read_index %= EventBuffer::BUFFER_SIZE;
    
    return true;
}

} // namespace IPC
} // namespace FM2K 