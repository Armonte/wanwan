#include <windows.h>
#include <iostream>
#include <vector>
#include <filesystem>
#include <string>
#include <cassert>

// SDL2 includes for controller support
#include <SDL.h>
#include <SDL_gamecontroller.h>

#pragma comment(lib, "SDL2.lib")
#pragma comment(lib, "SDL2main.lib")

namespace fs = std::filesystem;

class FM2KFramestep {
private:
    static constexpr DWORD HOOK_ADDRESS = 0x004146D0;  // process_game_inputs function
    static constexpr BYTE ORIGINAL_INSTRUCTION = 0x53; // PUSH EBX
    static constexpr BYTE BREAKPOINT_INSTRUCTION = 0xCC; // INT3
    
    // SDL Controller constants
    static constexpr SDL_GameControllerButton PAUSE_BUTTON = SDL_CONTROLLER_BUTTON_BACK;
    static constexpr SDL_GameControllerButton CONTINUE_BUTTON = SDL_CONTROLLER_BUTTON_A;
    
    HANDLE processHandle = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION processInfo = {};
    SDL_GameController* controllers[4] = {}; // Support up to 4 controllers
    bool isPaused = false;
    bool pauseButtonReleased = true;
    
public:
    FM2KFramestep() {
        // Initialize SDL2 with controller support
        SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
    
    ~FM2KFramestep() {
        cleanup();
        SDL_Quit();
    }
    
    bool findAndLaunchGame() {
        std::string exePath;
        
        // Look for .kgt files with matching .exe files
        for (const auto& entry : fs::directory_iterator(fs::current_path())) {
            if (entry.path().extension() == ".kgt") {
                auto exeFile = entry.path();
                exeFile.replace_extension(".exe");
                
                if (fs::exists(exeFile)) {
                    if (!exePath.empty()) {
                        std::cerr << "Error: Multiple potential game executables found.\n";
                        return false;
                    }
                    exePath = exeFile.string();
                    std::cout << "Found game executable: " << exePath << "\n";
                }
            }
        }
        
        if (exePath.empty()) {
            std::cerr << "Error: No game executables found.\n";
            return false;
        }
        
        return launchGame(exePath);
    }
    
private:
    bool launchGame(const std::string& exePath) {
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        
        // Launch with debug privileges
        BOOL result = CreateProcessA(
            exePath.c_str(),           // lpApplicationName
            nullptr,                   // lpCommandLine
            nullptr,                   // lpProcessAttributes
            nullptr,                   // lpThreadAttributes
            FALSE,                     // bInheritHandles
            DEBUG_ONLY_THIS_PROCESS,   // dwCreationFlags
            nullptr,                   // lpEnvironment
            nullptr,                   // lpCurrentDirectory
            &si,                       // lpStartupInfo
            &processInfo               // lpProcessInformation
        );
        
        if (!result) {
            std::cerr << "Failed to create process. Error: " << GetLastError() << "\n";
            return false;
        }
        
        processHandle = processInfo.hProcess;
        std::cout << "Game launched with PID: " << processInfo.dwProcessId << "\n";
        return true;
    }
    
    bool installHook() {
        // Read original instruction
        BYTE originalByte;
        SIZE_T bytesRead;
        
        if (!ReadProcessMemory(processHandle, (LPCVOID)HOOK_ADDRESS, &originalByte, 1, &bytesRead)) {
            std::cerr << "Failed to read original instruction\n";
            return false;
        }
        
        if (originalByte != ORIGINAL_INSTRUCTION) {
            std::cout << "Warning: Expected PUSH EBX (0x53), found 0x" << std::hex << (int)originalByte << "\n";
        }
        
        // Install breakpoint
        BYTE breakpoint = BREAKPOINT_INSTRUCTION;
        SIZE_T bytesWritten;
        
        if (!WriteProcessMemory(processHandle, (LPVOID)HOOK_ADDRESS, &breakpoint, 1, &bytesWritten)) {
            std::cerr << "Failed to install hook\n";
            return false;
        }
        
        std::cout << "Hook installed at 0x" << std::hex << HOOK_ADDRESS << "\n";
        return true;
    }
    
    void handleControllerInput() {
        SDL_Event event;
        
        if (!isPaused) {
            // Poll for pause input when not paused
            while (SDL_PollEvent(&event)) {
                processControllerEvent(event);
                if (isPaused) break; // Stop processing if we just paused
            }
        } else {
            // Wait for continue input when paused
            std::cout << "Game paused - Press A to continue or Back to step\n";
            while (isPaused) {
                SDL_WaitEvent(&event);
                processControllerEvent(event);
            }
        }
    }
    
    void processControllerEvent(const SDL_Event& event) {
        switch (event.type) {
            case SDL_CONTROLLERBUTTONUP:
                if (event.cbutton.button == PAUSE_BUTTON) {
                    pauseButtonReleased = true;
                }
                break;
                
            case SDL_CONTROLLERBUTTONDOWN:
                if (event.cbutton.button == PAUSE_BUTTON && pauseButtonReleased) {
                    if (isPaused) {
                        // Step one frame
                        isPaused = false;
                    } else {
                        // Pause
                        isPaused = true;
                    }
                    pauseButtonReleased = false;
                }
                else if (event.cbutton.button == CONTINUE_BUTTON && isPaused) {
                    // Continue running
                    isPaused = false;
                    std::cout << "Resuming game\n";
                }
                break;
                
            case SDL_CONTROLLERDEVICEADDED:
                // Open newly connected controller
                if (event.cdevice.which < 4) {
                    controllers[event.cdevice.which] = SDL_GameControllerOpen(event.cdevice.which);
                    if (controllers[event.cdevice.which]) {
                        std::cout << "Controller " << event.cdevice.which << " connected\n";
                    }
                }
                break;
                
            case SDL_CONTROLLERDEVICEREMOVED:
                // Handle controller disconnection
                for (int i = 0; i < 4; i++) {
                    if (controllers[i] && SDL_GameControllerGetAttached(controllers[i]) == SDL_FALSE) {
                        SDL_GameControllerClose(controllers[i]);
                        controllers[i] = nullptr;
                        std::cout << "Controller " << i << " disconnected\n";
                    }
                }
                break;
        }
    }
    
    void simulateOriginalInstruction(HANDLE threadHandle) {
        // Get thread context
        WOW64_CONTEXT context = {};
        context.ContextFlags = WOW64_CONTEXT_FULL;
        
        if (!Wow64GetThreadContext(threadHandle, &context)) {
            std::cerr << "Failed to get thread context\n";
            return;
        }
        
        // Simulate "PUSH EBX" instruction:
        // 1. Decrement ESP by 4
        // 2. Write EBX value to [ESP]
        context.Esp -= 4;
        
        // Write EBX to the stack location
        SIZE_T bytesWritten;
        if (!WriteProcessMemory(processHandle, (LPVOID)context.Esp, &context.Ebx, 4, &bytesWritten)) {
            std::cerr << "Failed to write to stack\n";
        }
        
        // Update thread context
        if (!Wow64SetThreadContext(threadHandle, &context)) {
            std::cerr << "Failed to set thread context\n";
        }
    }
    
public:
    void runDebugLoop() {
        DEBUG_EVENT debugEvent;
        
        std::cout << "Starting debug loop...\n";
        std::cout << "Controls:\n";
        std::cout << "  Back button: Pause/Step one frame\n";
        std::cout << "  A button: Continue from pause\n\n";
        
        while (true) {
            if (!WaitForDebugEvent(&debugEvent, INFINITE)) {
                std::cerr << "WaitForDebugEvent failed\n";
                break;
            }
            
            switch (debugEvent.dwDebugEventCode) {
                case CREATE_PROCESS_DEBUG_EVENT:
                    std::cout << "Process created, installing hook...\n";
                    installHook();
                    break;
                    
                case EXCEPTION_DEBUG_EVENT: {
                    auto* exception = &debugEvent.u.Exception;
                    DWORD exceptionAddress = (DWORD)exception->ExceptionRecord.ExceptionAddress;
                    
                    if (exceptionAddress == HOOK_ADDRESS) {
                        // Our breakpoint hit - handle frame stepping
                        simulateOriginalInstruction(processInfo.hThread);
                        handleControllerInput();
                        
                        if (isPaused) {
                            std::cout << "Frame " << std::dec << "paused at input processing\n";
                        }
                    }
                    break;
                }
                
                case EXIT_PROCESS_DEBUG_EVENT:
                    std::cout << "Process exited\n";
                    return;
                    
                default:
                    break;
            }
            
            // Continue execution
            ContinueDebugEvent(
                debugEvent.dwProcessId,
                debugEvent.dwThreadId,
                DBG_EXCEPTION_HANDLED
            );
        }
    }
    
private:
    void cleanup() {
        // Close controllers
        for (auto& controller : controllers) {
            if (controller) {
                SDL_GameControllerClose(controller);
                controller = nullptr;
            }
        }
        
        // Close process handles
        if (processInfo.hThread != INVALID_HANDLE_VALUE) {
            CloseHandle(processInfo.hThread);
        }
        if (processHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(processHandle);
        }
    }
};

int main() {
    std::cout << "FM2K Framestep Tool (C++ Version)\n";
    std::cout << "Based on Thorns' original Rust implementation\n\n";
    
    FM2KFramestep framestep;
    
    if (!framestep.findAndLaunchGame()) {
        std::cerr << "Failed to find and launch game\n";
        return 1;
    }
    
    framestep.runDebugLoop();
    
    std::cout << "Framestep tool exiting\n";
    return 0;
} 