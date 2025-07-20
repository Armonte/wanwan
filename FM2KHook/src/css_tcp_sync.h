#pragma once

#include <cstdint>
#include <thread>
#include <atomic>
#include <SDL3_net/SDL_net.h>

namespace FM2K {
namespace CSS {

// Simple TCP-based CSS state synchronization
// Runs alongside GekkoNet to provide real-time cursor updates
class TCPCursorSync {
public:
    TCPCursorSync();
    ~TCPCursorSync();
    
    // Initialize TCP sync (port = base_port + 100)
    bool Initialize(bool is_host, uint16_t base_port, const char* remote_ip);
    void Shutdown();
    
    // Send local cursor state
    void SendCursorUpdate(uint32_t cursor_x, uint32_t cursor_y, uint32_t character_id, uint32_t confirmed);
    
    // Get latest remote cursor state
    void GetRemoteCursor(uint32_t& cursor_x, uint32_t& cursor_y, uint32_t& character_id, uint32_t& confirmed);
    
    bool IsConnected() const { return connected_.load(); }
    
private:
    struct CursorPacket {
        uint32_t magic;      // 0xC5550000 for validation
        uint32_t cursor_x;
        uint32_t cursor_y;
        uint32_t character_id;
        uint32_t confirmed;
        uint32_t checksum;   // Simple sum of fields
    };
    
    // Network threads
    void ServerThread();
    void ClientThread();
    void ReceiveThread();
    
    // Connection info
    bool is_host_;
    uint16_t port_;
    std::string remote_ip_;
    
    // SDL_net objects
    NET_Server* server_;
    NET_StreamSocket* stream_socket_;
    NET_Address* remote_address_;
    std::atomic<bool> connected_;
    std::atomic<bool> running_;
    
    // Threads
    std::thread network_thread_;
    std::thread receive_thread_;
    
    // Latest cursor states
    CursorPacket local_cursor_;
    CursorPacket remote_cursor_;
    std::atomic<uint32_t> remote_update_counter_;
    
    // Thread safety (using SDL mutex for consistency)
    SDL_Mutex* cursor_mutex_;
};

// Global instance
extern TCPCursorSync g_tcp_cursor_sync;

} // namespace CSS
} // namespace FM2K