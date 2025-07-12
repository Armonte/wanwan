#include "FM2K_GameInstance.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // Parse command line arguments (like OnlineSession example)
    if (argc != 4) {
        std::cout << "Usage: " << argv[0] << " <player_index> <local_port> <remote_address>" << std::endl;
        std::cout << "Example: " << argv[0] << " 0 7000 127.0.0.1:7001" << std::endl;
        std::cout << "Example: " << argv[0] << " 1 7001 127.0.0.1:7000" << std::endl;
        return 1;
    }
    
    int player_index = std::stoi(argv[1]);
    uint16_t local_port = static_cast<uint16_t>(std::stoi(argv[2]));
    std::string remote_address = argv[3];
    
    std::cout << "FM2K GekkoNet Test Launcher" << std::endl;
    std::cout << "Player: " << player_index << std::endl;
    std::cout << "Local Port: " << local_port << std::endl;
    std::cout << "Remote Address: " << remote_address << std::endl;
    
    // Initialize SDL
    if (SDL_Init(SDL_INIT_EVENTS) != 0) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        return 1;
    }
    
    // Create game instance
    FM2KGameInstance game_instance;
    
    if (!game_instance.Initialize()) {
        std::cerr << "Failed to initialize game instance" << std::endl;
        SDL_Quit();
        return 1;
    }
    
    // Set up GekkoNet networking (like OnlineSession example)
    std::string game_exe_path = "C:\\Games\\FM2K\\fm2k.exe";  // Adjust path as needed
    
    std::cout << "Launching game with GekkoNet networking..." << std::endl;
    
    // Launch game with networking configuration
    if (!game_instance.LaunchWithNetworking(player_index, local_port, remote_address, game_exe_path)) {
        std::cerr << "Failed to launch game with networking" << std::endl;
        SDL_Quit();
        return 1;
    }
    
    std::cout << "Game launched successfully!" << std::endl;
    std::cout << "Waiting for game to initialize..." << std::endl;
    
    // Wait for game to start
    SDL_Delay(2000);
    
    // Main loop - monitor game process
    bool running = true;
    while (running && game_instance.IsRunning()) {
        // Process SDL events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
                break;
            }
        }
        
        // Process DLL events
        game_instance.ProcessDLLEvents();
        
        // Log network status every few seconds
        static uint32_t last_log_time = 0;
        uint32_t current_time = SDL_GetTicks();
        if (current_time - last_log_time > 5000) {  // Log every 5 seconds
            std::cout << "Game is running (PID: " << game_instance.GetProcessId() << ")" << std::endl;
            last_log_time = current_time;
        }
        
        SDL_Delay(100);  // 10 FPS monitoring
    }
    
    std::cout << "Game process ended or launcher terminated" << std::endl;
    
    // Cleanup
    game_instance.Terminate();
    SDL_Quit();
    
    return 0;
}