#include <windows.h>
#include <iostream>
#include <vector>
#include <filesystem>
#include <string>
#include <cassert>

// SDL3 includes for controller support
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#pragma comment(lib, "SDL3.lib")

namespace fs = std::filesystem;

class FM2KFramestepSDL3 {
private:
    static constexpr DWORD HOOK_ADDRESS = 0x004146D0;  // process_game_inputs function
    static constexpr BYTE ORIGINAL_INSTRUCTION = 0x53; // PUSH EBX
    static constexpr BYTE BREAKPOINT_INSTRUCTION = 0xCC; // INT3
    
    // SDL3 Controller constants (note the new naming)
    static constexpr SDL_GamepadButton PAUSE_BUTTON = SDL_GAMEPAD_BUTTON_BACK;
    static constexpr SDL_GamepadButton CONTINUE_BUTTON = SDL_GAMEPAD_BUTTON_A;
    
    HANDLE processHandle = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION processInfo = {};
    std::vector<SDL_Gamepad*> controllers; // Dynamic controller management
    bool isPaused = false;
    bool pauseButtonReleased = true;
    
public:
    FM2KFramestepSDL3() {
        // Initialize SDL3 with gamepad support
        if (!SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS)) {
            std::cerr << "Failed to initialize SDL3: " << SDL_GetError() << std::endl;
            return;
        }
        
        // SDL3 hints for background controller support
        SDL_SetHint("SDL_JOYSTICK_THREAD", "1");
        SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        
        initializeControllers();
    }
    
    ~FM2KFramestepSDL3() {
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
    void initializeControllers() {
        // SDL3 improved gamepad enumeration
        int count;
        SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
        
        if (gamepads) {
            std::cout << "Found " << count << " gamepad(s)\n";
            
            for (int i = 0; i < count; i++) {
                SDL_Gamepad* gamepad = SDL_OpenGamepad(gamepads[i]);
                if (gamepad && SDL_GamepadConnected(gamepad)) {
                    controllers.push_back(gamepad);
                    
                    // SDL3 properties system for gamepad info
                    SDL_PropertiesID props = SDL_GetGamepadProperties(gamepad);
                    const char* name = SDL_GetStringProperty(props, "SDL.gamepad.name", "Unknown Gamepad");
                    std::cout << "Connected: " << name << std::endl;
                }
            }
            
            SDL_free(gamepads);
        }
    }
    
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
                if (SDL_WaitEvent(&event)) {
                    processControllerEvent(event);
                }
            }
        }
    }
    
    void processControllerEvent(const SDL_Event& event) {
        switch (event.type) {
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (event.gbutton.button == PAUSE_BUTTON) {
                    pauseButtonReleased = true;
                }
                break;
                
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (event.gbutton.button == PAUSE_BUTTON && pauseButtonReleased) {
                    if (isPaused) {
                        // Step one frame
                        isPaused = false;
                        std::cout << "Stepping one frame\n";
                    } else {
                        // Pause
                        isPaused = true;
                        std::cout << "Game paused\n";
                    }
                    pauseButtonReleased = false;
                }
                else if (event.gbutton.button == CONTINUE_BUTTON && isPaused) {
                    // Continue running
                    isPaused = false;
                    std::cout << "Resuming game\n";
                }
                break;
                
            case SDL_EVENT_GAMEPAD_ADDED:
                // SDL3 improved device management
                {
                    SDL_Gamepad* gamepad = SDL_OpenGamepad(event.gdevice.which);
                    if (gamepad && SDL_GamepadConnected(gamepad)) {
                        controllers.push_back(gamepad);
                        
                        // Get gamepad info using properties system
                        SDL_PropertiesID props = SDL_GetGamepadProperties(gamepad);
                        const char* name = SDL_GetStringProperty(props, "SDL.gamepad.name", "Unknown");
                        std::cout << "Gamepad connected: " << name << std::endl;
                    }
                }
                break;
                
            case SDL_EVENT_GAMEPAD_REMOVED:
                // Handle gamepad disconnection
                {
                    auto it = std::find_if(controllers.begin(), controllers.end(),
                        [&](SDL_Gamepad* gamepad) {
                            return SDL_GetGamepadID(gamepad) == event.gdevice.which;
                        });
                    
                    if (it != controllers.end()) {
                        SDL_CloseGamepad(*it);
                        controllers.erase(it);
                        std::cout << "Gamepad disconnected\n";
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
        std::cout << "Controls (SDL3 Enhanced):\n";
        std::cout << "  Back button: Pause/Step one frame\n";
        std::cout << "  A button: Continue from pause\n";
        std::cout << "  Dynamic controller detection enabled\n\n";
        
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
                            // Frame counter for debugging
                            static uint32_t frameCount = 0;
                            std::cout << "Frame " << ++frameCount << " paused at input processing\n";
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
    
    // SDL3 Enhanced features
    void displayGamepadInfo() {
        std::cout << "\n=== Connected Gamepads (SDL3) ===\n";
        
        for (size_t i = 0; i < controllers.size(); i++) {
            SDL_Gamepad* gamepad = controllers[i];
            if (!SDL_GamepadConnected(gamepad)) continue;
            
            SDL_PropertiesID props = SDL_GetGamepadProperties(gamepad);
            
            // Get gamepad information using SDL3 properties
            const char* name = SDL_GetStringProperty(props, "SDL.gamepad.name", "Unknown");
            const char* vendor = SDL_GetStringProperty(props, "SDL.gamepad.vendor", "Unknown");
            const char* product = SDL_GetStringProperty(props, "SDL.gamepad.product", "Unknown");
            
            std::cout << "Gamepad " << i << ":\n";
            std::cout << "  Name: " << name << "\n";
            std::cout << "  Vendor: " << vendor << "\n";
            std::cout << "  Product: " << product << "\n";
            std::cout << "  Connected: " << (SDL_GamepadConnected(gamepad) ? "Yes" : "No") << "\n";
        }
        std::cout << "================================\n\n";
    }
    
private:
    void cleanup() {
        // Close all gamepads
        for (auto& gamepad : controllers) {
            if (gamepad) {
                SDL_CloseGamepad(gamepad);
            }
        }
        controllers.clear();
        
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
    std::cout << "FM2K Framestep Tool (SDL3 Version)\n";
    std::cout << "Enhanced with SDL3 gamepad improvements\n";
    std::cout << "Based on Thorns' original Rust implementation\n\n";
    
    FM2KFramestepSDL3 framestep;
    
    // Display gamepad information
    framestep.displayGamepadInfo();
    
    if (!framestep.findAndLaunchGame()) {
        std::cerr << "Failed to find and launch game\n";
        return 1;
    }
    
    framestep.runDebugLoop();
    
    std::cout << "Framestep tool exiting\n";
    return 0;
} 