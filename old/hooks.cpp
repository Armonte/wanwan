/**
 * Practice Mode V2 - Clean Hook System
 * 
 * Extracted from the working simple implementation.
 * Confirmed addresses:
 * - AI_DetermineNextAction (0x41c850) - CONFIRMED WORKING
 * - AI_CharacterSpecificStrategy (0x424970) - CONFIRMED WORKING
 */

#include "hooks.hpp"
#include <windows.h>
#include <cstdio>
#include "../../third_party/include/minhook/minhook.h"

namespace argentum::practice {

// Static instance for singleton
static HookSystem* g_instance = nullptr;

// ===========================================================================
// HOOK FUNCTIONS (Exact addresses from working implementation)
// ===========================================================================

int __cdecl HookSystem::AI_DetermineNextActionHook(unsigned char* aiActionTable, int aiStateTable, int aiSubStateTable) {
    HookSystem& instance = getInstance();
    
    // Call practice mode callback if registered
    if (instance.m_aiOverrideCallback) {
        int overrideAction = instance.m_aiOverrideCallback(aiActionTable, aiStateTable, aiSubStateTable);
        if (overrideAction >= 0) {
            return overrideAction;  // Practice mode is handling this
        }
    }
    
    // Fall back to original AI
    if (instance.m_originalAI_DetermineNextAction) {
        return instance.m_originalAI_DetermineNextAction(aiActionTable, aiStateTable, aiSubStateTable);
    }
    
    return 255;  // Safe fallback - ACTION_NEUTRAL_IDLE
}

int __cdecl HookSystem::AI_CharacterSpecificStrategyHook() {
    HookSystem& instance = getInstance();
    
    // Call character AI callback if registered
    if (instance.m_characterAICallback) {
        int result = instance.m_characterAICallback();
        if (result >= 0) {
            return result;
        }
    }
    
    // Call original character AI
    if (instance.m_originalAI_CharacterSpecificStrategy) {
        return instance.m_originalAI_CharacterSpecificStrategy();
    }
    
    return 0;  // Safe fallback - no specific action
}

void __cdecl HookSystem::HandlePlayerAttackHook(int playerIndex, int attackType, int damage, int hitFlag) {
    HookSystem& instance = getInstance();
    
    // Call combat analysis callback if registered
    if (instance.m_combatAnalysisCallback) {
        instance.m_combatAnalysisCallback(playerIndex, attackType, damage, hitFlag);
    }
    
    // Call original function
    if (instance.m_originalHandlePlayerAttack) {
        instance.m_originalHandlePlayerAttack(playerIndex, attackType, damage, hitFlag);
    }
}

// ===========================================================================
// HOOK SYSTEM IMPLEMENTATION
// ===========================================================================

HookSystem& HookSystem::getInstance() {
    if (!g_instance) {
        g_instance = new HookSystem();
    }
    return *g_instance;
}

bool HookSystem::install(uintptr_t baseAddr, size_t moduleSize) {
    if (m_installed) {
        printf("PRACTICE MODE HOOKS: Already installed, skipping\n");
        return true;  // Already installed
    }
    
    printf("PRACTICE MODE HOOKS: Installing practice mode hooks...\n");
    
    m_baseAddress = baseAddr;
    
    // Hook AI_DetermineNextAction at 0x41c850 (confirmed working)
    void* genericAIAddr = reinterpret_cast<void*>(baseAddr + AI_DETERMINE_NEXT_ACTION_OFFSET);
    MH_STATUS status1 = MH_CreateHook(
        genericAIAddr,
        reinterpret_cast<void*>(AI_DetermineNextActionHook),
        reinterpret_cast<void**>(&m_originalAI_DetermineNextAction)
    );
    
    if (status1 != MH_OK) {
        printf("PRACTICE MODE HOOKS: Failed to create AI_DetermineNextAction hook - status: %d (addr: %p)\n", 
               status1, genericAIAddr);
        if (status1 == MH_ERROR_ALREADY_CREATED) {
            printf("PRACTICE MODE HOOKS: Hook already exists - previous uninstall may have failed!\n");
        }
    }
    
    // Hook AI_CharacterSpecificStrategy at 0x424970 (confirmed working)
    void* characterAIAddr = reinterpret_cast<void*>(baseAddr + AI_CHARACTER_SPECIFIC_STRATEGY_OFFSET);
    MH_STATUS status2 = MH_CreateHook(
        characterAIAddr,
        reinterpret_cast<void*>(AI_CharacterSpecificStrategyHook),
        reinterpret_cast<void**>(&m_originalAI_CharacterSpecificStrategy)
    );
    
    if (status2 != MH_OK) {
        printf("PRACTICE MODE HOOKS: Failed to create AI_CharacterSpecificStrategy hook - status: %d (addr: %p)\n", 
               status2, characterAIAddr);
        if (status2 == MH_ERROR_ALREADY_CREATED) {
            printf("PRACTICE MODE HOOKS: Hook already exists - previous uninstall may have failed!\n");
        }
    }
    
    // Hook handlePlayerAttack at 0x4220c0 (? Phase 1.3: Combat Analysis)
    void* combatAddr = reinterpret_cast<void*>(baseAddr + HANDLE_PLAYER_ATTACK_OFFSET);
    MH_STATUS status3 = MH_CreateHook(
        combatAddr,
        reinterpret_cast<void*>(HandlePlayerAttackHook),
        reinterpret_cast<void**>(&m_originalHandlePlayerAttack)
    );
    
    if (status3 != MH_OK) {
        printf("PRACTICE MODE HOOKS: Failed to create HandlePlayerAttack hook - status: %d (addr: %p)\n", 
               status3, combatAddr);
        if (status3 == MH_ERROR_ALREADY_CREATED) {
            printf("PRACTICE MODE HOOKS: Hook already exists - previous uninstall may have failed!\n");
        }
    }
    
    if (status1 != MH_OK || status2 != MH_OK || status3 != MH_OK) {
        printf("PRACTICE MODE HOOKS: Failed to create hooks - attempting cleanup and retry...\n");
        
        // Try to clean up any partially created hooks
        if (status1 == MH_ERROR_ALREADY_CREATED) {
            MH_RemoveHook(genericAIAddr);
        }
        if (status2 == MH_ERROR_ALREADY_CREATED) {
            MH_RemoveHook(characterAIAddr);
        }
        if (status3 == MH_ERROR_ALREADY_CREATED) {
            MH_RemoveHook(combatAddr);
        }
        
        return false;
    }
    
    // Enable hooks - but only enable the specific hooks we just created
    MH_STATUS enableStatus1 = MH_EnableHook(genericAIAddr);
    MH_STATUS enableStatus2 = MH_EnableHook(characterAIAddr);
    MH_STATUS enableStatus3 = MH_EnableHook(combatAddr);
    
    if (enableStatus1 != MH_OK || enableStatus2 != MH_OK || enableStatus3 != MH_OK) {
        printf("PRACTICE MODE HOOKS: Failed to enable hooks - enable1: %d, enable2: %d, enable3: %d\n", 
               enableStatus1, enableStatus2, enableStatus3);
        return false;
    }
    
    m_installed = true;
    printf("PRACTICE MODE HOOKS: Successfully installed practice mode hooks\n");
    return true;
}

void HookSystem::uninstall() {
    if (!m_installed) {
        return;
    }
    
    printf("PRACTICE MODE HOOKS: Uninstalling practice mode hooks...\n");
    
    // FIXED: Properly disable AND remove hooks
    if (m_originalAI_DetermineNextAction) {
        void* hookAddr = reinterpret_cast<void*>(m_baseAddress + AI_DETERMINE_NEXT_ACTION_OFFSET);
        MH_STATUS disableStatus = MH_DisableHook(hookAddr);
        MH_STATUS removeStatus = MH_RemoveHook(hookAddr);  // CRITICAL: Remove the hook completely
        printf("PRACTICE MODE HOOKS: AI_DetermineNextAction - disable: %d, remove: %d\n", disableStatus, removeStatus);
        m_originalAI_DetermineNextAction = nullptr;
    }
    
    if (m_originalAI_CharacterSpecificStrategy) {
        void* hookAddr = reinterpret_cast<void*>(m_baseAddress + AI_CHARACTER_SPECIFIC_STRATEGY_OFFSET);
        MH_STATUS disableStatus = MH_DisableHook(hookAddr);
        MH_STATUS removeStatus = MH_RemoveHook(hookAddr);  // CRITICAL: Remove the hook completely
        printf("PRACTICE MODE HOOKS: AI_CharacterSpecificStrategy - disable: %d, remove: %d\n", disableStatus, removeStatus);
        m_originalAI_CharacterSpecificStrategy = nullptr;
    }
    
    if (m_originalHandlePlayerAttack) {
        void* hookAddr = reinterpret_cast<void*>(m_baseAddress + HANDLE_PLAYER_ATTACK_OFFSET);
        MH_STATUS disableStatus = MH_DisableHook(hookAddr);
        MH_STATUS removeStatus = MH_RemoveHook(hookAddr);  // CRITICAL: Remove the hook completely
        printf("PRACTICE MODE HOOKS: HandlePlayerAttack - disable: %d, remove: %d\n", disableStatus, removeStatus);
        m_originalHandlePlayerAttack = nullptr;
    }
    
    clearCallbacks();
    m_installed = false;
    
    printf("PRACTICE MODE HOOKS: Practice mode hooks uninstalled completely\n");
}

bool HookSystem::isInstalled() const {
    return m_installed;
}

void HookSystem::setAIOverrideCallback(AIOverrideCallback callback) {
    m_aiOverrideCallback = callback;
}

void HookSystem::setCharacterAICallback(CharacterAICallback callback) {
    m_characterAICallback = callback;
}

void HookSystem::setCombatAnalysisCallback(CombatAnalysisCallback callback) {
    m_combatAnalysisCallback = callback;
}

void HookSystem::clearCallbacks() {
    m_aiOverrideCallback = nullptr;
    m_characterAICallback = nullptr;
    m_combatAnalysisCallback = nullptr;
}

// ===========================================================================
// UTILITY FUNCTIONS
// ===========================================================================

namespace Hooks {
    bool install(uintptr_t baseAddr, size_t moduleSize) {
        return HookSystem::getInstance().install(baseAddr, moduleSize);
    }
    
    void uninstall() {
        HookSystem::getInstance().uninstall();
    }
    
    bool isActive() {
        return HookSystem::getInstance().isInstalled();
    }
    
    void setAIOverride(std::function<int(unsigned char*, int, int)> callback) {
        HookSystem::getInstance().setAIOverrideCallback(callback);
    }
}

} // namespace argentum::practice