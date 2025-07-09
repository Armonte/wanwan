#pragma once

#include "FM2K_Integration.h"
#include <memory>


class LocalSession : public ISession {
public:
    LocalSession();
    ~LocalSession();

    // ISession interface implementation
    bool Start(const NetworkConfig& config) override;
    void Stop() override;
    void Update() override;
    bool IsActive() const override;

    void AddLocalInput(uint32_t input) override;
    void AddBothInputs(uint32_t p1_input, uint32_t p2_input) override;
    SessionMode GetSessionMode() const override;
    NetworkStats GetStats() const override;

    void SetGameInstance(FM2KGameInstance* instance) override;

private:
    FM2KGameInstance* game_instance_;
}; 