#include "simple_css_sync.h"
#include <SDL3/SDL_log.h>
#include <cstring>
#include <cstdlib>

SimpleCSSSync::SimpleCSSSync()
    : is_host_(false)
    , port_(0)
    , socket_(nullptr)
    , connected_(false)
    , state_changed_(false)
{
    memset(&local_state_, 0, sizeof(local_state_));
    memset(&remote_state_, 0, sizeof(remote_state_));
    local_state_.magic = 0xC55C55C5;
    remote_state_.magic = 0xC55C55C5;
}

SimpleCSSSync::~SimpleCSSSync() {
    Shutdown();
}

bool SimpleCSSSync::Initialize(bool is_host, uint16_t base_port, const char* remote_ip) {
    is_host_ = is_host;
    port_ = base_port + 200;  // CSS port = GekkoNet port + 200 (avoid +100 conflict)
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Initializing %s on port %u", 
        is_host ? "HOST" : "CLIENT", port_);
    
    if (is_host_) {
        // Host: Create server and accept connection
        NET_Server* server = NET_CreateServer(nullptr, port_);
        if (!server) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Failed to create server: %s", SDL_GetError());
            return false;
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Server created on port %u, waiting for client...", port_);
        
        // Wait for client with timeout (NET_AcceptClient is non-blocking)
        int timeout_ms = 10000; // 10 seconds
        int start_time = SDL_GetTicks();
        socket_ = nullptr;
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: HOST waiting for CLIENT to connect...");
        
        while (!socket_ && (SDL_GetTicks() - start_time) < timeout_ms) {
            bool accepted = NET_AcceptClient(server, &socket_);
            if (accepted && socket_) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: CLIENT connected successfully - socket: %p", socket_);
                break;
            }
            // No client ready yet, wait a bit
            SDL_Delay(10);
        }
        
        NET_DestroyServer(server); // Clean up server
        
        if (!socket_) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: No client connected within timeout");
            return false;
        }
        
    } else {
        // Client: Connect to host
        NET_Address* address = NET_ResolveHostname(remote_ip);
        if (!address) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Failed to resolve hostname %s: %s", 
                remote_ip, SDL_GetError());
            return false;
        }
        
        // Wait for address resolution to complete (SDL3_net is async)
        int resolve_result = NET_WaitUntilResolved(address, 5000); // 5 second timeout
        if (resolve_result <= 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Address resolution timeout or failed for %s", remote_ip);
            NET_UnrefAddress(address);
            return false;
        }
        
        socket_ = NET_CreateClient(address, port_);
        NET_UnrefAddress(address);
        
        if (!socket_) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Failed to connect to %s:%u: %s", 
                remote_ip, port_, SDL_GetError());
            return false;
        }
        
        // Wait for connection to establish
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: CLIENT waiting for connection to establish...");
        int wait_result = NET_WaitUntilConnected(socket_, 5000); // 5 second timeout
        if (wait_result <= 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Connection timeout or error - wait_result: %d", wait_result);
            NET_DestroyStreamSocket(socket_);
            socket_ = nullptr;
            return false;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: CLIENT connection established successfully");
    }
    
    connected_ = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Connected successfully!");
    return true;
}

void SimpleCSSSync::UpdateLocalState(uint8_t cursor_x, uint8_t cursor_y, uint8_t confirmed, uint8_t color_button) {
    // Update appropriate player based on our role
    if (is_host_) {
        local_state_.p1_cursor_x = cursor_x;
        local_state_.p1_cursor_y = cursor_y;
        local_state_.p1_confirmed = confirmed;
        // Only update button if it changed (to avoid persistent sending)
        if (color_button != local_state_.p1_color_button) {
            local_state_.p1_color_button = color_button;
            state_changed_ = true;
        }
    } else {
        local_state_.p2_cursor_x = cursor_x;
        local_state_.p2_cursor_y = cursor_y;
        local_state_.p2_confirmed = confirmed;
        // Only update button if it changed (to avoid persistent sending)
        if (color_button != local_state_.p2_color_button) {
            local_state_.p2_color_button = color_button;
            state_changed_ = true;
        }
    }
    
    // Always mark changed for cursor/confirm updates
    if (!state_changed_) {
        state_changed_ = true;
    }
}

bool SimpleCSSSync::SendUpdate() {
    if (!connected_ || !state_changed_) {
        return true; // Nothing to send
    }
    
    // Send our local state in one shot
    int bytes_sent = NET_WriteToStreamSocket(socket_, &local_state_, sizeof(CSSMessage));
    
    if (bytes_sent == sizeof(CSSMessage)) {
        // Success - full message sent
        state_changed_ = false;
        return true;
    } else if (bytes_sent < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Send failed: %s", SDL_GetError());
        connected_ = false;
        return false;
    } else {
        // Partial send - this shouldn't happen with such small messages
        // Just log and retry next frame rather than spamming logs
        return true; // Don't mark state_changed_ = false, will retry
    }
}

bool SimpleCSSSync::ReceiveUpdate() {
    if (!connected_) {
        return false;
    }
    
    // Check if data is available (non-blocking using 0ms timeout)
    void* sockets[] = { socket_ };
    int ready = NET_WaitUntilInputAvailable(sockets, 1, 0); // 0ms = non-blocking
    if (ready <= 0) {
        return true; // No data available, but not an error
    }
    
    // Read incoming message
    CSSMessage incoming;
    int bytes_received = NET_ReadFromStreamSocket(socket_, &incoming, sizeof(CSSMessage));
    
    if (bytes_received <= 0) {
        if (bytes_received < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Receive failed: %s", SDL_GetError());
        }
        connected_ = false;
        return false;
    }
    
    if (bytes_received != sizeof(CSSMessage)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS: Partial receive: %d/%d bytes", 
            bytes_received, (int)sizeof(CSSMessage));
        return false; // Wait for complete message
    }
    
    // Validate magic number
    if (incoming.magic != 0xC55C55C5) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS: Invalid magic number: 0x%08X", incoming.magic);
        return false;
    }
    
    // Update remote state with received data
    if (is_host_) {
        // Host receives P2 state from client
        remote_state_.p2_cursor_x = incoming.p2_cursor_x;
        remote_state_.p2_cursor_y = incoming.p2_cursor_y;
        remote_state_.p2_confirmed = incoming.p2_confirmed;
        remote_state_.p2_color_button = incoming.p2_color_button;
    } else {
        // Client receives P1 state from host
        remote_state_.p1_cursor_x = incoming.p1_cursor_x;
        remote_state_.p1_cursor_y = incoming.p1_cursor_y;
        remote_state_.p1_confirmed = incoming.p1_confirmed;
        remote_state_.p1_color_button = incoming.p1_color_button;
    }
    
    return true;
}

bool SimpleCSSSync::BothPlayersReady() const {
    // Check if we have both confirmations (non-zero means confirmed)
    bool p1_ready = (is_host_ ? local_state_.p1_confirmed : remote_state_.p1_confirmed) != 0;
    bool p2_ready = (is_host_ ? remote_state_.p2_confirmed : local_state_.p2_confirmed) != 0;
    
    return p1_ready && p2_ready;
}

void SimpleCSSSync::Shutdown() {
    if (socket_) {
        NET_DestroyStreamSocket(socket_);
        socket_ = nullptr;
    }
    
    connected_ = false;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Shutdown complete");
}