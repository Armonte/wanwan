#pragma once
#include <windows.h>

namespace GAME {
    inline int* g_bPlayer2_IsReady = (int*)0x4eddca;
    inline int* g_game_paused = (int*)0x4701bc;

    // Functions and globals from initialize_game
    inline int (*initialize_game)() = (int (*)())0x4056c0;
    inline int (*ReadSessionProfileString)() = (int (*)())0x4148e0;
    inline int (*hit_judge_set_function)() = (int (*)())0x414930;
    inline void (*memory_clear)(void* ptr, int size) = (void (*)(void*, int))0x403300;
    inline int (*register_custom_controls)() = (int (*)())0x416530;
    inline void (*ClearGameDataBuffers)() = (void (*)())0x415170;
    inline int (*Net_Initialize)() = (int (*)())0x4029c0;
    inline int (*joy1_setup)() = (int (*)())0x414230;
    inline int (*joy2_setup)() = (int (*)())0x4142e0;
    inline int (*Initialize_DirectSound)() = (int (*)())0x403330;
    inline int (*ResetGameAndCreateManager)() = (int (*)())0x406970;

    inline void** decompressed_buffer = (void***)0x425a44;
    inline char* g_mci_drive_letter = (char*)0x41e408;
    inline BITMAPINFO* pbmi = (BITMAPINFO*)0x424298;
    inline void** compressed_buffer = (void***)0x424f60;
    inline int* g_player_data_slots = (int*)0x4d1d80;
    inline void** g_menu_resource_data = (void***)0x425a60;
    inline void** g_ui_graphics_data = (void***)0x445740;
    inline int* g_config_value5 = (int*)0x430108;
    inline int* g_dest_width = (int*)0x447f20;
    inline int* g_dest_height = (int*)0x447f24;
    inline HINSTANCE* g_hinstance = (HINSTANCE*)0x4701cc;
    inline const char** IconName = (const char***)0x41ec94;
    inline const char** aCupidMenu = (const char***)0x41ec9c;
    inline const char** g_window_class_name_KGT2KGAME = (const char***)0x41e7bc;
    inline const char** WindowName = (const char***)0x42477c;
    inline int* g_window_x = (int*)0x425a48;
    inline int* g_window_y = (int*)0x425a4c;
    inline HDC* g_hDeviceContext = (HDC*)0x421630;
    inline void** g_back_buffer_pixels = (void***)0x4246cc;
    inline BITMAPINFO* stru_421650 = (BITMAPINFO*)0x421650; // Placeholder name
    inline HDC* dword_421A78 = (HDC*)0x421A78; // Placeholder name
    inline HBITMAP* dword_421A7C = (HBITMAP*)0x421A7C; // Placeholder name
    inline void** ppvBits = (void***)0x421a84;
    inline HGDIOBJ* dword_421A80 = (HGDIOBJ*)0x421A80; // Placeholder name
    inline int* g_display_config = (int*)0x4d1d60;
    inline int* g_player_character_ids = (int*)0x4cf9e0;

    // Function addresses
    inline void (*initialize_directdraw_mode)(void* this_ptr) = (void (*)(void*))0x404980;

    // Global variable addresses
    inline int* g_graphics_init_counter = (int*)0x424770;
    inline int* g_graphics_busy_flag = (int*)0x42476c;
    inline int* g_dd_init_success_count = (int*)0x424774;
    inline int* g_graphics_mode = (int*)0x424704;
    inline HWND* g_hwnd_parent = (HWND*)0x4246f8;
    inline RECT* Rect = (RECT*)0x424f40;
    inline int* g_window_initialized = (int*)0x42475c;
    inline int* g_graphics_state = (int*)0x424768;
    inline RECT* g_cursor_clip_rect = (RECT*)0x4259e0;
    inline void** g_direct_draw = (void***)0x424758; // IDirectDraw**
    inline DDSURFACEDESC* g_dd_surface_desc = (DDSURFACEDESC*)0x46ff40;
    inline int* g_dd_surface_caps = (int*)0x46ff44; // Actually part of DDSURFACEDESC
    inline int* g_dd_surface_format = (int*)0x46ffa8; // Actually part of DDSURFACEDESC
    inline int* g_dd_buffer_count = (int*)0x46ff54; // Actually part of DDSURFACEDESC
    inline void** g_dd_primary_surface = (void***)0x424750; // IDirectDrawSurface**
    inline void** g_dd_back_buffer = (void***)0x424754; // IDirectDrawSurface**
    inline int* g_dd_init_success = (int*)0x424760;

} // namespace GAME

