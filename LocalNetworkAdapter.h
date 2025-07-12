#pragma once

#include "vendored/GekkoNet/GekkoLib/include/gekkonet.h"
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

// Local Network Adapter for simulating network communication between two local clients
// Uses shared memory to pass data between Host (Client 1) and Guest (Client 2)
class LocalNetworkAdapter {
public:
    enum Role {
        HOST = 0,   // Client 1 - sends to Guest buffer, reads from Host buffer
        GUEST = 1   // Client 2 - sends to Host buffer, reads from Guest buffer
    };

private:
    // Packet structure for shared memory communication
    struct NetworkPacket {
        uint32_t sequence_id;
        uint32_t data_length;
        uint64_t timestamp_us;  // For latency simulation
        char data[1024];        // Max packet size
    };

    // Shared memory buffer structure
    struct NetworkBuffer {
        uint32_t write_index;
        uint32_t read_index;
        uint32_t packet_count;
        NetworkPacket packets[64]; // Circular buffer for packets
    };

    Role role_;
    HANDLE shared_memory_handle_;
    NetworkBuffer* shared_memory_;
    std::string shared_memory_name_;
    
    // Simulation parameters
    uint32_t simulated_latency_ms_;
    float packet_loss_rate_;
    uint32_t jitter_variance_ms_;
    
    // Received packet buffer for this frame
    std::vector<GekkoNetResult*> received_packets_;
    std::mutex received_packets_mutex_;

public:
    LocalNetworkAdapter(Role role);
    ~LocalNetworkAdapter();
    
    bool Initialize();
    void Shutdown();
    
    // GekkoNetAdapter interface
    GekkoNetAdapter* GetAdapter() { return &adapter_; }
    
    // Network simulation
    void SetSimulatedLatency(uint32_t latency_ms) { simulated_latency_ms_ = latency_ms; }
    void SetPacketLossRate(float loss_rate) { packet_loss_rate_ = loss_rate; }
    void SetJitterVariance(uint32_t jitter_ms) { jitter_variance_ms_ = jitter_ms; }

private:
    GekkoNetAdapter adapter_;
    
    // Static callback functions for GekkoNet
    static void SendData(GekkoNetAddress* addr, const char* data, int length);
    static GekkoNetResult** ReceiveData(int* length);
    static void FreeData(void* data_ptr);
    
    // Instance methods
    void SendDataImpl(GekkoNetAddress* addr, const char* data, int length);
    GekkoNetResult** ReceiveDataImpl(int* length);
    void FreeDataImpl(void* data_ptr);
    
    // Helper methods
    bool ShouldDropPacket();
    uint32_t CalculateDelay();
    NetworkBuffer* GetSendBuffer();
    NetworkBuffer* GetReceiveBuffer();
    uint64_t GetMicroseconds();
    
    // Static instance tracking for callbacks
    static LocalNetworkAdapter* host_instance_;
    static LocalNetworkAdapter* guest_instance_;
    static LocalNetworkAdapter* GetInstance(Role role);
};