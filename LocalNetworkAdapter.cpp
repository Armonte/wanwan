#include "LocalNetworkAdapter.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <random>
#include <cstring>

// Static instance tracking
LocalNetworkAdapter* LocalNetworkAdapter::host_instance_ = nullptr;
LocalNetworkAdapter* LocalNetworkAdapter::guest_instance_ = nullptr;

LocalNetworkAdapter::LocalNetworkAdapter(Role role)
    : role_(role)
    , shared_memory_handle_(nullptr)
    , shared_memory_(nullptr)
    , simulated_latency_ms_(0)
    , packet_loss_rate_(0.0f)
    , jitter_variance_ms_(0)
{
    // Set up GekkoNetAdapter function pointers
    adapter_.send_data = SendData;
    adapter_.receive_data = ReceiveData;
    adapter_.free_data = FreeData;
    
    // Generate shared memory name
    shared_memory_name_ = "FM2K_LocalNetwork_SharedMemory";
    
    // Register this instance
    if (role == HOST) {
        host_instance_ = this;
    } else {
        guest_instance_ = this;
    }
}

LocalNetworkAdapter::~LocalNetworkAdapter() {
    Shutdown();
    
    // Unregister this instance
    if (role_ == HOST) {
        host_instance_ = nullptr;
    } else {
        guest_instance_ = nullptr;
    }
}

bool LocalNetworkAdapter::Initialize() {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LocalNetworkAdapter: Initializing %s", 
                role_ == HOST ? "HOST" : "GUEST");
    
    // Create or open shared memory for network communication
    size_t shared_memory_size = sizeof(NetworkBuffer) * 2; // Host buffer + Guest buffer
    
    shared_memory_handle_ = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(shared_memory_size),
        shared_memory_name_.c_str()
    );
    
    if (!shared_memory_handle_) {
        DWORD error = GetLastError();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LocalNetworkAdapter: Failed to create shared memory (error: %lu)", error);
        return false;
    }
    
    shared_memory_ = static_cast<NetworkBuffer*>(MapViewOfFile(
        shared_memory_handle_,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        shared_memory_size
    ));
    
    if (!shared_memory_) {
        DWORD error = GetLastError();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LocalNetworkAdapter: Failed to map shared memory (error: %lu)", error);
        CloseHandle(shared_memory_handle_);
        shared_memory_handle_ = nullptr;
        return false;
    }
    
    // Initialize buffers if we're the first to create them
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LocalNetworkAdapter: Initializing shared memory buffers");
        memset(shared_memory_, 0, shared_memory_size);
    }
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "LocalNetworkAdapter: Initialized successfully as %s", 
                role_ == HOST ? "HOST" : "GUEST");
    
    return true;
}

void LocalNetworkAdapter::Shutdown() {
    if (shared_memory_) {
        UnmapViewOfFile(shared_memory_);
        shared_memory_ = nullptr;
    }
    
    if (shared_memory_handle_) {
        CloseHandle(shared_memory_handle_);
        shared_memory_handle_ = nullptr;
    }
    
    // Clean up received packets
    std::lock_guard<std::mutex> lock(received_packets_mutex_);
    for (auto* packet : received_packets_) {
        delete[] static_cast<char*>(packet->data);
        delete packet;
    }
    received_packets_.clear();
}

// Static callback functions for GekkoNet
void LocalNetworkAdapter::SendData(GekkoNetAddress* addr, const char* data, int length) {
    // Use thread-local storage to determine which instance called this
    // Since each client runs in its own process, we can use a simple process-wide current instance
    static thread_local LocalNetworkAdapter* current_instance = nullptr;
    
    // Try to find the correct instance based on which one is initialized
    if (host_instance_ && host_instance_->shared_memory_) {
        current_instance = host_instance_;
    } else if (guest_instance_ && guest_instance_->shared_memory_) {
        current_instance = guest_instance_;
    }
    
    if (current_instance) {
        current_instance->SendDataImpl(addr, data, length);
    }
}

GekkoNetResult** LocalNetworkAdapter::ReceiveData(int* length) {
    // Use the same approach to find the current instance
    static thread_local LocalNetworkAdapter* current_instance = nullptr;
    
    if (host_instance_ && host_instance_->shared_memory_) {
        current_instance = host_instance_;
    } else if (guest_instance_ && guest_instance_->shared_memory_) {
        current_instance = guest_instance_;
    }
    
    if (current_instance) {
        return current_instance->ReceiveDataImpl(length);
    }
    
    *length = 0;
    return nullptr;
}

void LocalNetworkAdapter::FreeData(void* data_ptr) {
    // Free data is stateless - we can call it on any instance
    if (host_instance_) {
        host_instance_->FreeDataImpl(data_ptr);
    } else if (guest_instance_) {
        guest_instance_->FreeDataImpl(data_ptr);
    }
}

LocalNetworkAdapter* LocalNetworkAdapter::GetInstance(Role role) {
    return (role == HOST) ? host_instance_ : guest_instance_;
}

// Instance implementation methods
void LocalNetworkAdapter::SendDataImpl(GekkoNetAddress* addr, const char* data, int length) {
    if (!shared_memory_ || length <= 0 || length > sizeof(NetworkPacket::data)) {
        return;
    }
    
    // Check for packet loss simulation
    if (ShouldDropPacket()) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "LocalNetworkAdapter: Simulating packet loss");
        return;
    }
    
    NetworkBuffer* send_buffer = GetSendBuffer();
    if (!send_buffer) {
        return;
    }
    
    // Calculate simulated delay
    uint64_t send_time = GetMicroseconds() + (CalculateDelay() * 1000);
    
    // Add packet to circular buffer
    uint32_t write_index = send_buffer->write_index % 64;
    NetworkPacket* packet = &send_buffer->packets[write_index];
    
    packet->sequence_id = send_buffer->packet_count++;
    packet->data_length = static_cast<uint32_t>(length);
    packet->timestamp_us = send_time;
    memcpy(packet->data, data, length);
    
    // Update write index
    send_buffer->write_index = (send_buffer->write_index + 1) % 64;
    
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "LocalNetworkAdapter (%s): Sent packet %u, length %d", 
                 role_ == HOST ? "HOST" : "GUEST", packet->sequence_id, length);
}

GekkoNetResult** LocalNetworkAdapter::ReceiveDataImpl(int* length) {
    *length = 0;
    
    if (!shared_memory_) {
        return nullptr;
    }
    
    NetworkBuffer* receive_buffer = GetReceiveBuffer();
    if (!receive_buffer) {
        return nullptr;
    }
    
    // Clear previous frame's packets
    {
        std::lock_guard<std::mutex> lock(received_packets_mutex_);
        for (auto* packet : received_packets_) {
            delete[] static_cast<char*>(packet->data);
            delete packet;
        }
        received_packets_.clear();
    }
    
    uint64_t current_time = GetMicroseconds();
    std::vector<GekkoNetResult*> available_packets;
    
    // Check for packets that have "arrived" (accounting for simulated latency)
    while (receive_buffer->read_index != receive_buffer->write_index) {
        uint32_t read_index = receive_buffer->read_index % 64;
        NetworkPacket* packet = &receive_buffer->packets[read_index];
        
        // Check if packet has "arrived" based on simulated latency
        if (packet->timestamp_us <= current_time) {
            // Create GekkoNetResult
            GekkoNetResult* result = new GekkoNetResult;
            result->addr.data = nullptr;
            result->addr.size = 0;
            result->data_len = packet->data_length;
            result->data = new char[packet->data_length];
            memcpy(result->data, packet->data, packet->data_length);
            
            available_packets.push_back(result);
            
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "LocalNetworkAdapter (%s): Received packet %u, length %u", 
                         role_ == HOST ? "HOST" : "GUEST", packet->sequence_id, packet->data_length);
            
            // Move read index
            receive_buffer->read_index = (receive_buffer->read_index + 1) % 64;
        } else {
            // Packet hasn't "arrived" yet due to simulated latency
            break;
        }
    }
    
    if (available_packets.empty()) {
        return nullptr;
    }
    
    // Store packets and create result array
    {
        std::lock_guard<std::mutex> lock(received_packets_mutex_);
        received_packets_ = std::move(available_packets);
        *length = static_cast<int>(received_packets_.size());
        return received_packets_.data();
    }
}

void LocalNetworkAdapter::FreeDataImpl(void* data_ptr) {
    // Data will be cleaned up in the next ReceiveDataImpl call
    // or in destructor - GekkoNet just needs this function to exist
}

// Helper methods
bool LocalNetworkAdapter::ShouldDropPacket() {
    if (packet_loss_rate_ <= 0.0f) {
        return false;
    }
    
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    
    return dis(gen) < packet_loss_rate_;
}

uint32_t LocalNetworkAdapter::CalculateDelay() {
    uint32_t base_delay = simulated_latency_ms_;
    
    if (jitter_variance_ms_ > 0) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> jitter_dis(0, jitter_variance_ms_);
        base_delay += jitter_dis(gen);
    }
    
    return base_delay;
}

LocalNetworkAdapter::NetworkBuffer* LocalNetworkAdapter::GetSendBuffer() {
    if (!shared_memory_) return nullptr;
    
    // HOST sends to guest buffer (index 1), GUEST sends to host buffer (index 0)
    return &shared_memory_[role_ == HOST ? 1 : 0];
}

LocalNetworkAdapter::NetworkBuffer* LocalNetworkAdapter::GetReceiveBuffer() {
    if (!shared_memory_) return nullptr;
    
    // HOST receives from host buffer (index 0), GUEST receives from guest buffer (index 1)
    return &shared_memory_[role_ == HOST ? 0 : 1];
}

uint64_t LocalNetworkAdapter::GetMicroseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}