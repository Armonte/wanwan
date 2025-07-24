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
    
    // Verify structure size is as expected (16 bytes)
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: CSSMessage size = %d bytes", (int)sizeof(CSSMessage));
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
        
        // Host: Handle handshake from client
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: HOST waiting for CLIENT handshake...");
        
        int handshake_wait = 0;
        bool got_handshake = false;
        uint32_t handshake_msg = 0;
        
        while (handshake_wait < 3000) { // 3 second timeout (increased from 1s)
            void* sockets[] = { socket_ };
            int ready = NET_WaitUntilInputAvailable(sockets, 1, 100);
            
            if (ready > 0) {
                int bytes_received = NET_ReadFromStreamSocket(socket_, &handshake_msg, sizeof(handshake_msg));
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: HOST received %d bytes, expected %d", bytes_received, (int)sizeof(handshake_msg));
                
                if (bytes_received > 0) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: HOST received data: 0x%08X", handshake_msg);
                }
                
                if (bytes_received == sizeof(handshake_msg) && handshake_msg == 0xC5511A5D) {
                    // Send handshake response
                    NET_WriteToStreamSocket(socket_, &handshake_msg, sizeof(handshake_msg));
                    got_handshake = true;
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: HOST handshake successful after %dms", handshake_wait);
                    break;
                } else if (bytes_received < 0) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: HOST socket error: %s", SDL_GetError());
                    break;
                }
            }
            handshake_wait += 100;
        }
        
        if (!got_handshake) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: HOST handshake timeout");
            NET_DestroyStreamSocket(socket_);
            socket_ = nullptr;
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
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Socket created, starting connection to %s:%u", remote_ip, port_);
        
        // Always wait a bit for TCP connection to establish
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: CLIENT waiting for connection to establish...");
        int wait_result = NET_WaitUntilConnected(socket_, 1000); // 1 second timeout
        if (wait_result <= 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Connection not ready after 1s, trying handshake anyway");
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Connection established");
        }
        
        // Perform handshake to verify connection
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: CLIENT sending handshake...");
        uint32_t handshake = 0xC5511A5D; // CSS HAND
        
        // Wait longer for socket to be fully ready for writing
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Waiting for socket to be ready for writing...");
        
        // Check if socket is ready for writing with timeout
        int write_ready = 0;
        for (int i = 0; i < 10; i++) { // Try for up to 1 second
            void* write_sockets[] = { socket_ };
            // Use SDL_net's write readiness check (if available) or just wait
            SDL_Delay(100); // Wait 100ms between attempts
            write_ready = 1; // Assume ready after wait
            break;
        }
        
        if (!write_ready) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Socket not ready for writing after 1 second");
        }
        
        // Log socket state  
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Socket ptr: %p, attempting to send %d bytes", socket_, (int)sizeof(handshake));
        
        // Send the complete handshake in one call first, then retry if partial
        int bytes_sent = NET_WriteToStreamSocket(socket_, &handshake, sizeof(handshake));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Initial send attempt - sent %d bytes of %d", bytes_sent, (int)sizeof(handshake));
        
        if (bytes_sent != sizeof(handshake)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS: Partial send detected, trying retry approach...");
            
            // If partial send, try the retry approach
            int total_sent = (bytes_sent > 0) ? bytes_sent : 0;
            uint8_t* data = (uint8_t*)&handshake;
            
            for (int attempt = 0; attempt < 5 && total_sent < sizeof(handshake); attempt++) {
                SDL_Delay(50); // Wait between attempts
                
                int remaining = sizeof(handshake) - total_sent;
                int chunk_sent = NET_WriteToStreamSocket(socket_, data + total_sent, remaining);
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Retry attempt %d - sent %d bytes (total: %d/%d)", 
                           attempt + 1, chunk_sent, total_sent + (chunk_sent > 0 ? chunk_sent : 0), (int)sizeof(handshake));
                
                if (chunk_sent > 0) {
                    total_sent += chunk_sent;
                } else if (chunk_sent < 0) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Send error on retry: %s", SDL_GetError());
                    break;
                }
            }
            
            bytes_sent = total_sent;
        }
        
        if (bytes_sent != sizeof(handshake)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Failed to send handshake: %d bytes sent", bytes_sent);
            NET_DestroyStreamSocket(socket_);
            socket_ = nullptr;
            return false;
        }
        
        // Wait for handshake response
        void* sockets[] = { socket_ };
        int ready = NET_WaitUntilInputAvailable(sockets, 1, 1000); // 1 second timeout
        
        if (ready <= 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Handshake response timeout");
            NET_DestroyStreamSocket(socket_);
            socket_ = nullptr;
            return false;
        }
        
        uint32_t response = 0;
        int bytes_received = NET_ReadFromStreamSocket(socket_, &response, sizeof(response));
        
        if (bytes_received != sizeof(response) || response != 0xC5511A5D) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS: Invalid handshake response");
            NET_DestroyStreamSocket(socket_);
            socket_ = nullptr;
            return false;
        }
        
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: CLIENT handshake successful");
    }
    
    connected_ = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS: Connected successfully!");
    return true;
}

void SimpleCSSSync::UpdateLocalState(uint8_t cursor_x, uint8_t cursor_y, uint8_t confirmed, uint16_t color_button) {
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
        // Reduce logging frequency for partial receives
        static int partial_receive_counter = 0;
        if (++partial_receive_counter % 60 == 0) { // Log every second instead of every frame
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS: Partial receive: %d/%d bytes", 
                bytes_received, (int)sizeof(CSSMessage));
        }
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