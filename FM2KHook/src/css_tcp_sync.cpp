#include "css_tcp_sync.h"
#include <SDL3/SDL_log.h>
#include <cstring>

namespace FM2K {
namespace CSS {

// Global instance
TCPCursorSync g_tcp_cursor_sync;

TCPCursorSync::TCPCursorSync()
    : is_host_(false)
    , port_(0)
    , server_(nullptr)
    , stream_socket_(nullptr)
    , remote_address_(nullptr)
    , connected_(false)
    , running_(false)
    , cursor_mutex_(nullptr)
    , remote_update_counter_(0)
{
    memset(&local_cursor_, 0, sizeof(local_cursor_));
    memset(&remote_cursor_, 0, sizeof(remote_cursor_));
    local_cursor_.magic = 0xC5550000;
    remote_cursor_.magic = 0xC5550000;
}

TCPCursorSync::~TCPCursorSync() {
    Shutdown();
}

bool TCPCursorSync::Initialize(bool is_host, uint16_t base_port, const char* remote_ip) {
    if (running_.load()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Already initialized");
        return true;
    }
    
    if (!NET_Init()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Failed to initialize SDL_net");
        return false;
    }
    
    cursor_mutex_ = SDL_CreateMutex();
    if (!cursor_mutex_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Failed to create mutex");
        NET_Quit();
        return false;
    }
    
    is_host_ = is_host;
    port_ = base_port + 100;  // CSS sync port = GekkoNet port + 100
    remote_ip_ = remote_ip;
    running_ = true;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Initializing %s on port %u", 
        is_host ? "SERVER" : "CLIENT", port_);
    
    // Start network thread
    if (is_host_) {
        network_thread_ = std::thread(&TCPCursorSync::ServerThread, this);
    } else {
        network_thread_ = std::thread(&TCPCursorSync::ClientThread, this);
    }
    
    return true;
}

void TCPCursorSync::Shutdown() {
    if (!running_.load()) return;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Shutting down");
    
    running_ = false;
    connected_ = false;
    
    // Clean up sockets
    if (stream_socket_) {
        NET_DestroyStreamSocket(stream_socket_);
        stream_socket_ = nullptr;
    }
    
    if (server_) {
        NET_DestroyServer(server_);
        server_ = nullptr;
    }
    
    if (remote_address_) {
        NET_UnrefAddress(remote_address_);
        remote_address_ = nullptr;
    }
    
    // Wait for threads
    if (network_thread_.joinable()) {
        network_thread_.join();
    }
    
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    
    // Clean up SDL objects
    if (cursor_mutex_) {
        SDL_DestroyMutex(cursor_mutex_);
        cursor_mutex_ = nullptr;
    }
    
    NET_Quit();
}

void TCPCursorSync::SendCursorUpdate(uint32_t cursor_x, uint32_t cursor_y, uint32_t character_id, uint32_t confirmed) {
    if (!connected_.load() || !stream_socket_) return;
    
    SDL_LockMutex(cursor_mutex_);
    
    local_cursor_.cursor_x = cursor_x;
    local_cursor_.cursor_y = cursor_y;
    local_cursor_.character_id = character_id;
    local_cursor_.confirmed = confirmed;
    local_cursor_.checksum = cursor_x + cursor_y + character_id + confirmed + local_cursor_.magic;
    
    SDL_UnlockMutex(cursor_mutex_);
    
    // Send immediately with error checking
    bool success = NET_WriteToStreamSocket(stream_socket_, &local_cursor_, sizeof(CursorPacket));
    if (!success) {
        static uint32_t error_counter = 0;
        if (++error_counter % 10 == 0) {  // Log every 10th error to avoid spam
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Failed to send cursor update (error %u)", error_counter);
        }
        // Don't immediately disconnect - socket might recover
    }
}

void TCPCursorSync::GetRemoteCursor(uint32_t& cursor_x, uint32_t& cursor_y, uint32_t& character_id, uint32_t& confirmed) {
    SDL_LockMutex(cursor_mutex_);
    
    cursor_x = remote_cursor_.cursor_x;
    cursor_y = remote_cursor_.cursor_y;
    character_id = remote_cursor_.character_id;
    confirmed = remote_cursor_.confirmed;
    
    SDL_UnlockMutex(cursor_mutex_);
}

void TCPCursorSync::ServerThread() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Starting server on port %u", port_);
    
    server_ = NET_CreateServer(nullptr, port_);  // Listen on all interfaces
    if (!server_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Failed to create server");
        running_ = false;
        return;
    }
    
    while (running_.load()) {
        NET_StreamSocket* client = nullptr;
        if (NET_AcceptClient(server_, &client)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Client connected");
            
            stream_socket_ = client;
            connected_ = true;
            
            // Give the connection a moment to stabilize
            SDL_Delay(100);
            
            // Start receive thread
            receive_thread_ = std::thread(&TCPCursorSync::ReceiveThread, this);
            
            // Give receive thread time to start
            SDL_Delay(100);
            
            // Wait for connection to close or run for a reasonable time
            int connection_time = 0;
            while (connected_.load() && running_.load() && connection_time < 300000) { // 5 minute max
                SDL_Delay(100);
                connection_time += 100;
            }
            
            if (receive_thread_.joinable()) {
                receive_thread_.join();
            }
            
            if (stream_socket_) {
                NET_DestroyStreamSocket(stream_socket_);
                stream_socket_ = nullptr;
            }
        }
        
        SDL_Delay(100);  // Check for new connections every 100ms
    }
}

void TCPCursorSync::ClientThread() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Connecting to %s:%u", remote_ip_.c_str(), port_);
    
    // Resolve address
    remote_address_ = NET_ResolveHostname(remote_ip_.c_str());
    if (!remote_address_) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Failed to resolve %s", remote_ip_.c_str());
        running_ = false;
        return;
    }
    
    // Wait for resolution
    if (NET_WaitUntilResolved(remote_address_, 5000) != 1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Address resolution timeout");
        running_ = false;
        return;
    }
    
    while (running_.load()) {
        // Try to connect
        stream_socket_ = NET_CreateClient(remote_address_, port_);
        if (stream_socket_) {
            // Wait for connection
            if (NET_WaitUntilConnected(stream_socket_, 5000) == 1) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Connected to server");
                connected_ = true;
                
                // Give the connection a moment to stabilize
                SDL_Delay(200);
                
                // Don't send test packet immediately - let the receive thread start first
                
                // Start receive thread
                receive_thread_ = std::thread(&TCPCursorSync::ReceiveThread, this);
                
                // Wait for connection to close or run for a reasonable time
                int connection_time = 0;
                while (connected_.load() && running_.load() && connection_time < 300000) { // 5 minute max
                    SDL_Delay(100);
                    connection_time += 100;
                }
                
                if (receive_thread_.joinable()) {
                    receive_thread_.join();
                }
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Connection timeout");
            }
            
            NET_DestroyStreamSocket(stream_socket_);
            stream_socket_ = nullptr;
        }
        
        connected_ = false;
        
        if (running_.load()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Retrying connection in 1 second...");
            SDL_Delay(1000);
        }
    }
}

void TCPCursorSync::ReceiveThread() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Starting receive thread");
    
    // Give connection time to stabilize
    SDL_Delay(200);
    
    while (connected_.load() && running_.load()) {
        // Don't check connection status every loop - it's unreliable
        // Instead rely on read operations to detect disconnections
        
        CursorPacket packet;
        
        // Use non-blocking read to avoid hanging
        int bytes_read = NET_ReadFromStreamSocket(stream_socket_, &packet, sizeof(CursorPacket));
        
        if (bytes_read == sizeof(CursorPacket)) {
            // Validate packet
            uint32_t expected_checksum = packet.cursor_x + packet.cursor_y + packet.character_id + packet.confirmed + packet.magic;
            if (packet.magic == 0xC5550000 && packet.checksum == expected_checksum) {
                SDL_LockMutex(cursor_mutex_);
                
                remote_cursor_ = packet;
                remote_update_counter_.fetch_add(1);
                
                SDL_UnlockMutex(cursor_mutex_);
                
                // Log occasionally
                static uint32_t log_counter = 0;
                if (++log_counter % 100 == 0) {
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, 
                        "CSS TCP: Received cursor update - pos(%u,%u) char:%u conf:%u", 
                        packet.cursor_x, packet.cursor_y, packet.character_id, packet.confirmed);
                }
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Invalid packet (magic=0x%08X, checksum mismatch)", packet.magic);
            }
        } else if (bytes_read == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Connection closed gracefully by remote");
            connected_ = false;
            break;
        } else if (bytes_read < 0) {
            // No data available - this is normal for non-blocking reads, don't log it
            // Don't treat no data as an error - this is normal for non-blocking reads
            // connected_ = false;
            // break;
        } else if (bytes_read > 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Partial packet received (%d bytes)", bytes_read);
        }
        
        // Small delay to prevent busy waiting - longer when no data
        SDL_Delay(bytes_read > 0 ? 5 : 50);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "CSS TCP: Receive thread ended");
}

} // namespace CSS
} // namespace FM2K