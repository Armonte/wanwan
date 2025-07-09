#include "hook_manager.hpp"
#include <MinHook.h>
#include <cstdio>
#include <cstring>

namespace argentum::hooks::core {
    
    // Static member definitions
    HookManager::HookInfo HookManager::s_installedHooks[MAX_HOOKS] = {};
    size_t HookManager::s_hookCount = 0;
    
    bool HookManager::InstallJumpHook(uintptr_t targetAddress, void* hookFunction, const char* description) {
        if (!ValidateHookTarget(targetAddress)) {
            LogHookInstallation(description, targetAddress, false);
            return false;
        }
        
        return WriteJumpInstruction(targetAddress, (uintptr_t)hookFunction, 0xE9) &&
               TrackHook(targetAddress, hookFunction, description ? description : "Jump Hook");
    }
    
    bool HookManager::InstallCallHook(uintptr_t targetAddress, void* hookFunction, const char* description) {
        if (!ValidateHookTarget(targetAddress)) {
            LogHookInstallation(description, targetAddress, false);
            return false;
        }
        
        return WriteJumpInstruction(targetAddress, (uintptr_t)hookFunction, 0xE8) &&
               TrackHook(targetAddress, hookFunction, description ? description : "Call Hook");
    }
    
    bool HookManager::InstallMinHook(uintptr_t targetAddress, void* hookFunction, void** originalFunction, const char* description) {
        MH_STATUS createStatus = MH_CreateHook((LPVOID)targetAddress, hookFunction, originalFunction);
        if (createStatus != MH_OK) {
            LogHookInstallation(description, targetAddress, false);
            printf("ERROR: MH_CreateHook failed with status %d\n", createStatus);
            return false;
        }
        
        MH_STATUS enableStatus = MH_EnableHook((LPVOID)targetAddress);
        if (enableStatus != MH_OK) {
            LogHookInstallation(description, targetAddress, false);
            printf("ERROR: MH_EnableHook failed with status %d\n", enableStatus);
            return false;
        }
        
        TrackHook(targetAddress, hookFunction, description ? description : "MinHook");
        LogHookInstallation(description, targetAddress, true);
        return true;
    }
    
    bool HookManager::WriteJumpInstruction(uintptr_t address, uintptr_t target, BYTE opcode) {
        DWORD relativeOffset = CalculateRelativeOffset(address, target);
        
        DWORD oldProtection;
        if (!ChangeMemoryProtection((void*)address, 5, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            return false;
        }
        
        // Write jump/call opcode
        *(BYTE*)address = opcode;
        
        // Write relative offset
        *(DWORD*)(address + 1) = relativeOffset;
        
        bool success = RestoreMemoryProtection((void*)address, 5, oldProtection);
        
        // Flush instruction cache to ensure changes take effect
        FlushInstructionCache(GetCurrentProcess(), (void*)address, 5);
        
        return success;
    }
    
    DWORD HookManager::CalculateRelativeOffset(uintptr_t from, uintptr_t to) {
        // Calculate relative jump: destination - source - 5 (instruction size)
        return (DWORD)(to - from - 5);
    }
    
    bool HookManager::PatchBytes(uintptr_t address, const void* data, size_t size, const char* description) {
        if (!ValidateHookTarget(address, size)) {
            LogHookInstallation(description, address, false);
            return false;
        }
        
        DWORD oldProtection;
        if (!ChangeMemoryProtection((void*)address, size, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            LogHookInstallation(description, address, false);
            return false;
        }
        
        memcpy((void*)address, data, size);
        
        bool success = RestoreMemoryProtection((void*)address, size, oldProtection);
        FlushInstructionCache(GetCurrentProcess(), (void*)address, size);
        
        if (success) {
            TrackHook(address, nullptr, description ? description : "Byte Patch");
        }
        
        LogHookInstallation(description, address, success);
        return success;
    }
    
    bool HookManager::PatchNOP(uintptr_t address, size_t count, const char* description) {
        if (!ValidateHookTarget(address, count)) {
            LogHookInstallation(description, address, false);
            return false;
        }
        
        DWORD oldProtection;
        if (!ChangeMemoryProtection((void*)address, count, PAGE_EXECUTE_READWRITE, &oldProtection)) {
            LogHookInstallation(description, address, false);
            return false;
        }
        
        // Fill with NOP instructions (0x90)
        memset((void*)address, 0x90, count);
        
        bool success = RestoreMemoryProtection((void*)address, count, oldProtection);
        FlushInstructionCache(GetCurrentProcess(), (void*)address, count);
        
        if (success) {
            TrackHook(address, nullptr, description ? description : "NOP Patch");
        }
        
        LogHookInstallation(description, address, success);
        return success;
    }
    
    bool HookManager::ChangeMemoryProtection(void* address, size_t size, DWORD newProtection, DWORD* oldProtection) {
        DWORD tempOldProtection;
        DWORD* protectionPtr = oldProtection ? oldProtection : &tempOldProtection;
        
        return VirtualProtect(address, size, newProtection, protectionPtr) != FALSE;
    }
    
    bool HookManager::RestoreMemoryProtection(void* address, size_t size, DWORD oldProtection) {
        DWORD ignored;
        return VirtualProtect(address, size, oldProtection, &ignored) != FALSE;
    }
    
    bool HookManager::ValidateHookTarget(uintptr_t address, size_t requiredSize) {
        // Basic validation - check if address is readable
        if (IsBadReadPtr((void*)address, requiredSize)) {
            printf("ERROR: Hook target address 0x%p is not readable\n", (void*)address);
            return false;
        }
        
        // Check if address is in a reasonable range (basic heuristic)
        if (address < 0x400000 || address > 0x7FFFFFFF) {
            printf("ERROR: Hook target address 0x%p is outside expected range\n", (void*)address);
            return false;
        }
        
        return true;
    }
    
    void HookManager::LogHookInstallation(const char* description, uintptr_t address, bool success) {
        const char* status = success ? "SUCCESS" : "FAILED";
        const char* desc = description ? description : "Unknown Hook";
        
        printf("HOOK %s: %s at 0x%p\n", status, desc, (void*)address);
    }
    
    bool HookManager::TrackHook(uintptr_t address, void* hookFunction, const char* description) {
        if (s_hookCount >= MAX_HOOKS) {
            printf("WARNING: Maximum hook count reached, cannot track hook at 0x%p\n", (void*)address);
            return false;
        }
        
        HookInfo& info = s_installedHooks[s_hookCount++];
        info.address = address;
        info.hookFunction = hookFunction;
        info.isActive = true;
        
        // Copy description safely
        if (description) {
            strncpy_s(info.description, sizeof(info.description), description, _TRUNCATE);
        } else {
            strcpy_s(info.description, sizeof(info.description), "Unknown");
        }
        
        return true;
    }
    
    bool HookManager::RemoveHook(uintptr_t address) {
        // Find and deactivate the hook
        for (size_t i = 0; i < s_hookCount; ++i) {
            if (s_installedHooks[i].address == address && s_installedHooks[i].isActive) {
                s_installedHooks[i].isActive = false;
                printf("HOOK REMOVED: %s at 0x%p\n", s_installedHooks[i].description, (void*)address);
                return true;
            }
        }
        
        printf("WARNING: Hook at 0x%p not found for removal\n", (void*)address);
        return false;
    }
    
    void HookManager::CleanupAllHooks() {
        printf("HOOK CLEANUP: Cleaning up %zu installed hooks\n", s_hookCount);
        
        for (size_t i = 0; i < s_hookCount; ++i) {
            if (s_installedHooks[i].isActive) {
                s_installedHooks[i].isActive = false;
            }
        }
        
        s_hookCount = 0;
        printf("HOOK CLEANUP: All hooks cleaned up\n");
    }
    
    void HookManager::PrintHookStatistics() {
        size_t activeHooks = 0;
        for (size_t i = 0; i < s_hookCount; ++i) {
            if (s_installedHooks[i].isActive) {
                activeHooks++;
            }
        }
        
        printf("=== HOOK STATISTICS ===\n");
        printf("Total hooks installed: %zu\n", s_hookCount);
        printf("Active hooks: %zu\n", activeHooks);
        printf("Inactive hooks: %zu\n", s_hookCount - activeHooks);
        
        if (s_hookCount > 0) {
            printf("\nActive hooks:\n");
            for (size_t i = 0; i < s_hookCount; ++i) {
                if (s_installedHooks[i].isActive) {
                    printf("  [%zu] %s at 0x%p\n", i, s_installedHooks[i].description, (void*)s_installedHooks[i].address);
                }
            }
        }
        printf("========================\n");
    }
    
} // namespace argentum::hooks::core