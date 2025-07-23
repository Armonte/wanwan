#pragma once

#include <cstdint>
#include <SDL3_net/SDL_net.h>

// Simple CSS synchronization without threads
// Uses basic TCP socket for reliable cursor position sync

struct CSSMessage {
    uint32_t magic = 0xC55C55C5;  // "CSS CSS" magic number
    uint8_t p1_cursor_x;          // P1 cursor X (low byte of u32)
    uint8_t p1_cursor_y;          // P1 cursor Y (low byte of u32)
    uint8_t p1_confirmed;         // P1 confirmation status
    uint16_t p1_color_button;     // P1 color button (0x010, 0x020, 0x040, 0x080, 0x100, 0x200)
    uint8_t p2_cursor_x;          // P2 cursor X (low byte of u32)
    uint8_t p2_cursor_y;          // P2 cursor Y (low byte of u32)
    uint8_t p2_confirmed;         // P2 confirmation status
    uint16_t p2_color_button;     // P2 color button (0x010, 0x020, 0x040, 0x080, 0x100, 0x200)
};

class SimpleCSSSync {
private:
    bool is_host_;
    uint16_t port_;
    NET_StreamSocket* socket_;
    bool connected_;
    
    // Local state
    CSSMessage local_state_;
    CSSMessage remote_state_;
    bool state_changed_;

public:
    SimpleCSSSync();
    ~SimpleCSSSync();
    
    // Initialize as host or client
    bool Initialize(bool is_host, uint16_t base_port, const char* remote_ip = "127.0.0.1");
    
    // Update local CSS state
    void UpdateLocalState(uint8_t cursor_x, uint8_t cursor_y, uint8_t confirmed, uint16_t color_button = 0);
    
    // Send/receive CSS updates (non-blocking)
    bool SendUpdate();
    bool ReceiveUpdate();
    
    // Check if both players are ready for battle
    bool BothPlayersReady() const;
    
    // Get remote player state
    const CSSMessage& GetRemoteState() const { return remote_state_; }
    const CSSMessage& GetLocalState() const { return local_state_; }
    
    // Clear remote button state after injection
    void ClearRemoteButton() {
        if (is_host_) {
            remote_state_.p2_color_button = 0;
        } else {
            remote_state_.p1_color_button = 0;
        }
    }
    
    // Cleanup
    void Shutdown();
    
    bool IsConnected() const { return connected_; }
};