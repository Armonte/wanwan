// TODO: First, we will need to check that the current code works on local machine.
//   Second, implement delay

// Licensed under GPLV3

// Based on https://github.com/kovidomi/BBTAG-Improvement-Mod/blob/master/src/Hooks/HookManager.cpp

#include <windows.h>
#include <libloaderapi.h>
#include <psapi.h>
#include <MinHook.h>

#include "cry_and_die.hpp"
#include "netplay.hpp"
#include "replayer.hpp"
#include "recorder.hpp"
#include "display_font_sprite_hook_impl.h"
#include "address_definitions.h"  

#include "caster_lib/Logger.hpp"
#include "caster_lib/SocketManager.hpp"
#include "caster_lib/TimerManager.hpp"
#include "argentum.hpp"
#include "ctx/ctx.hpp"
#include "engine/engine.hpp"
#include <thread>
#include <chrono>
#include "debug_utils.h"
#include "memory_mapper.h"
#include "hooks/initgame_hook.hpp"

// SDL3 DirectDraw compatibility layer includes
#include "hooks/impl/sdl3_context.hpp"
#include "hooks/impl/surface_management.hpp"
#include "hooks/impl/palette_debug.hpp"
#include "simple_input_hooks.h"

using UpdatePointerArrayFunc = int(__cdecl*)(unsigned int, DWORD*);
using IsRectangleWithinScreenFunc = int(__cdecl*)(int, int, int, int);

// Global variable to store the palette hook address for cleanup
static DWORD g_getPaletteEntryAddr = 0;


void addFrmSpriteToRenderBufferHOOK(uintptr_t baseAddr, size_t moduleSize);
void clearGlobalAnimControlHOOK(uintptr_t baseAddr, size_t moduleSize);
void updateRenderStateHOOK(uintptr_t baseAddr, size_t moduleSize);
void UpdatePointerArrayHOOK(uintptr_t baseAddr, size_t moduleSize);
void resetResourceCounterHOOK(uintptr_t baseAddr, size_t moduleSize);
void ResetGameVariable_todoHOOK(uintptr_t baseAddr, size_t moduleSize);
void cleanupResourcesHOOK(uintptr_t baseAddr, size_t moduleSize);
void ReallocateGlobalResourceArrayHOOK(uintptr_t baseAddr, size_t moduleSize);
void ReallocateRenderBufferHOOK(uintptr_t baseAddr, size_t moduleSize);
void hookTimeStall(uintptr_t baseAddr, size_t moduleSize);
void processVSEDataHOOK(uintptr_t baseAddr, size_t moduleSize);
void processVSEEntryHOOK(uintptr_t baseAddr, size_t moduleSize);
void initDirectDrawHOOK(uintptr_t baseAddr, size_t moduleSize);
void CreateMainWindowHOOK(uintptr_t baseAddr, size_t moduleSize);
void UpdateColorInformationHOOK(uintptr_t baseAddr, size_t moduleSize);
void InitializeResourceHandlersHOOK(uintptr_t baseAddr, size_t moduleSize);
void UpdatePaletteEntriesHOOK(uintptr_t baseAddr, size_t moduleSize);
void HandleGlobalPaletteHOOK(uintptr_t baseAddr, size_t moduleSize);
void ProcessScreenUpdatesAndResourcesHOOK(uintptr_t baseAddr, size_t moduleSize);

// Additional SDL3-compatible hook function declarations
void InitializeWindowHOOK(uintptr_t baseAddr, size_t moduleSize);
void isGraphicsSystemInitializedHOOK(uintptr_t baseAddr, size_t moduleSize);

// DirectDraw to SDL3 hook forward declaration
namespace argentum::hooks {
    int __cdecl initDirectDraw_new(int isFullScreen, void* windowHandle);
    HWND __cdecl CreateMainWindow_new(int displayMode, HINSTANCE hInstance, int nCmdShow);
    int __cdecl UpdateColorInformation_new();
    HRESULT __cdecl initializeResourceHandlers_new();
    int __cdecl UpdatePaletteEntries_new(int startIndex, unsigned int entryCount, char* colorData, unsigned int colorFormat);
    int __cdecl ProcessScreenUpdatesAndResources_new();
    bool InstallPaletteHooks(DWORD getPaletteEntryAddr);
    void UninstallPaletteHooks(DWORD getPaletteEntryAddr);
    void ForcePaletteUpdate(); // Add this for the timer approach
}

extern "C" {
  void fullScreenCrashFix();
  void fullScreenCrashFix2();
  void fullScreenCrashFix3();
  void fullScreenCrashFix4();
  
  void skipDoubleInstanceCheck();
  void displayFontSpriteHookWrapper();
  void displayFontSpriteHookImpl();
  uint32_t originalDisplayFontSprite(int32_t, int32_t, int32_t, int32_t, int32_t, const char*, ...);
  int __cdecl addFrmSpriteToRenderBuffer_new(
    int renderingLayer,
    int tileImageId,
    int blendValue,
    int flipBits,
    int posX,
    int posY,
    int width,
    int height,
    int additionalParam);
  int __cdecl ClearGlobalAnimControl_new();
  int __cdecl updateRenderState_new();
  int __cdecl UpdatePointerArray_new(unsigned int index, DWORD* newentry);
  void __cdecl resetResourceCounter_new();
  void __cdecl ResetGameVariable_todo_new();
  void __cdecl cleanupResources_new();
  int __cdecl ReallocateGlobalResourceArray_new(int newSize);
  int __cdecl ReallocateRenderBuffer_new(int newSize);
  int __cdecl InternalFrmSprite_new(
    DWORD* spriteMetaData,
    int textureID,
    unsigned short colorInfo,
    unsigned int renderingLayer,
    short offsetX,
    short offsetY,
    int alphaFactor,
    char renderingFlags,
    int minAlphaThreshold,
    int maxAlphaThreshold);
  
  // VSE Data processing hooks
  int __cdecl processVSEDataHook(unsigned __int16 frameId, int vse_Data_pointer, 
                                unsigned __int16 currentFrame, __int16 flag);
  int __cdecl processVSEEntryHook(unsigned __int16 frameIndex, int vseDataPtr, 
                                 unsigned __int16 currentFrame, __int16 flag);
  int __cdecl processVSEentry_new(unsigned __int16 frameIndex, int vseDataPtr, 
                                 unsigned __int16 currentFrame, __int16 flag);

  typedef DWORD (__cdecl *TimeStallFunc)(int delayTime);
  
  extern void *addressThatZxInputComparedAgainst;
  extern void *addressThatAsInputComparedAgainst;

  void ProcessGameFrameHookWrapper();
  typedef void (__fastcall *MainGameLoopFunc)(unsigned int delayTimeMs);
  TimeStallFunc originalTimeStall = nullptr;
  MainGameLoopFunc originalMainGameLoop = nullptr;
  int ProcessGameFrameHook();
  

  #define TIME_STALL_ADDRESS 0x2d840 
  #define addFrmSpriteToRenderBuffer_ADDRESS 0x2CD40
  #define updateRenderState_ADDRESS 0x2CC50 
  #define UpdatePointerArray_ADDRESS 0x2CE10   
  #define resetResourceCounter_ADDRESS 0x2CC10  
  #define cleanupResources_ADDRESS 0x2CC20  
  #define ResetGameVariable_todo_ADDRESS 0x2CC30  
  #define ReallocateGlobalResourceArray_ADDRESS 0x2CBC0
  #define ReallocateRenderBuffer_ADDRESS 0x2CCC0
  #define INIT_DIRECTDRAW_ADDRESS 0x6580       // 0x406580 - initDirectDraw
  #define CREATE_MAIN_WINDOW_ADDRESS 0x5EF0    // 0x405EF0 - CreateMainWindow
  #define UPDATE_COLOR_INFORMATION_ADDRESS 0x126C0  // 0x4126C0 - UpdateColorInformation
  #define INITIALIZE_RESOURCE_HANDLERS_ADDRESS 0x12670  // 0x412670 - initializeResourceHandlers
  #define PROCESS_SCREEN_UPDATES_ADDRESS 0x124D0       // 0x4124D0 - ProcessScreenUpdatesAndResources

// Additional SDL3-compatible function addresses  
#define updateRenderState_ADDRESS 0x2CC50     // 0x42CC50 - updateRenderState
#define INITIALIZE_WINDOW_ADDRESS 0x2D440       // 0x42D440 - InitializeWindow
#define IS_GRAPHICS_INITIALIZED_ADDRESS 0x2D400 // 0x42D400 - isGraphicsSystemInitialized
  
  // VSE Data processing addresses
  #define PROCESS_VSE_DATA_ADDRESS 0x11680     // 0x411680 - process_VSE_Data
  #define PROCESS_VSE_ENTRY_ADDRESS 0x2FB70    // 0x42FB70 - processVSEentry
  
  // Palette function addresses
  #define GET_PALETTE_ENTRY_ADDRESS 0x2BBF0    // 0x42BBF0 - GetPaletteEntry
  #define UPDATE_PALETTE_ENTRIES_ADDRESS 0x2BA10  // 0x42BA10 - UpdatePaletteEntries
  DWORD timeStallHook(int delayTime);  
  void initGameSpeedMonitor();
  int BattleGameLoopHook();
}


enum class PatchType {
  Call, Jmp
};

static int readDelay() {
  char buf[32];
  if (!GetEnvironmentVariableA("MOONLIGHT_CASTER_NET_DELAY", buf, sizeof(buf)))
    cryAndDie("Missing delay info");

  int delay;
  if (sscanf(buf, "%d", &delay) != 1) cryAndDie("Non-numeric delay value");

  return delay;
}

static bool setClipboard ( const std::string& str )
{
    if ( OpenClipboard ( 0 ) )
    {
        HGLOBAL clipbuffer = GlobalAlloc ( GMEM_DDESHARE, str.size() + 1 );
        char *buffer = ( char * ) GlobalLock ( clipbuffer );
        strcpy ( buffer, LPCSTR ( str.c_str() ) );
        GlobalUnlock ( clipbuffer );
        EmptyClipboard();
        SetClipboardData ( CF_TEXT, clipbuffer );
        CloseClipboard();
	return true;
    }
    return false;
}

unsigned char *findPattern(unsigned char * const base,
			   size_t baseSz,
			   const unsigned char * const pattern,
			   size_t patternSz) {
  unsigned char *toPatch = nullptr;
  for (auto ptr = base; ptr < base + baseSz - patternSz; ++ptr) {
    if (memcmp(ptr, pattern, patternSz) == 0) {
      if (toPatch)
	cryAndDie("I have found it twice :( :( :(");
      toPatch = ptr;
    }
  }
  return toPatch;
}

void patch(unsigned char* const base,                // code segment start
	   size_t baseSz,                            // code segment size
	   const unsigned char * const pattern,      // pattern to look for in code segment
	   size_t patternSz,                         // size of pattern we look for
	   ptrdiff_t patchOffset,                    // offset to patch from pattern location
	   void(*fix)(),                             // code to which the patch will CALL/JMP
	   size_t nops,                              // Number of NOPs to place after CALL/JMP
	   PatchType patchType,                      // JMP or CALL?
	   unsigned char * const original = nullptr) // If not null, will write pre patch code here
{
  unsigned char * const patPtr = findPattern(base, baseSz, pattern, patternSz);
  unsigned char * const toPatch = patPtr + patchOffset; // the diff should point to the code that will be replaced

  char printfBuf[100];

  if (!patPtr) {
    wsprintfA(printfBuf, "I have NOT found the Moon Lights 2 draw routine!!! (%d)", (int)patternSz);
    cryAndDie(printfBuf);
  }

  ptrdiff_t relAddr = (unsigned char*)fix - toPatch - 5;
  DWORD curProtection;
  if (!VirtualProtect(toPatch, 5 + nops, PAGE_READWRITE, &curProtection)) {
    LPSTR messageBuffer;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		  NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
    MessageBoxA(0, "VirtualProtect failed: ", "hook dll", MB_OK | MB_ICONERROR);
    cryAndDie(messageBuffer);
  }
  if (original) memcpy(original, toPatch, 5 + nops);
  switch (patchType) {
  case PatchType::Call:
    *toPatch = 0xE8; // CALL
    break;
  case PatchType::Jmp:
    *toPatch = 0xE9; // JMP
    break;
  default:
    cryAndDie("Unknown patchType");
  }    
  memcpy(toPatch + 1, &relAddr, 4); // fullScreenCrashFix
  if (nops) memset(toPatch + 5, 0x90, nops);
  if (DWORD ignored; !VirtualProtect(toPatch, 5 + nops, curProtection, &ignored))
    cryAndDie("VirtualProtect2 failed");
  FlushInstructionCache(GetCurrentProcess(), toPatch, 5 + nops);
}
				

void applyFullScreenCrashFix(unsigned char* const base, size_t size)
{
  // drp = drawRoutinePattern
  const unsigned char drp1[] = {
    0x89, 0x44, 0x24, 0x10,
    0x89, 0x44, 0x24, 0x0c,
    0x8d, 0x44, 0x24, 0x0c
  };
  const unsigned char drp2[] = {
    0x8d, 0x44, 0x24, 0x34,
    0x6a, 0x00,
    0x6a, 0x00,
    0x8b, 0x0d,
  };
  const unsigned char drp3[] = {
    0x68, 0x00, 0x00, 0x00, 0x01,
    0xc7, 0x84, 0x24, 0xa8, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00
  };
  const unsigned char drp4[] = {
    0x68, 0x00, 0x00, 0x00, 0x01,
    0xc7, 0x84, 0x24, 0xa8, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00
  };

  patch(base, size, drp1, sizeof(drp1), (0x412522 - 0x4124fd), fullScreenCrashFix, 0, PatchType::Call); 
  patch(base, size, drp2, sizeof(drp2), (0x412596 - 0x412584), fullScreenCrashFix2, 0, PatchType::Call);
  patch(base, size, drp3, sizeof(drp3), (0x4126a0 - 0x412686), fullScreenCrashFix3, 0, PatchType::Call);
  patch(base, size, drp4, sizeof(drp4), (0x4126AE - 0x412686), fullScreenCrashFix4, 1, PatchType::Call);
}

void applyAllowDoubleInstance(unsigned char* const base, size_t size) {
  const unsigned char pat[] = {
    0x8B, 0xF0,
    0x85, 0xF6,
    0x74, 0x63,
    0xC7, 0x44, 0x24, 0x04, 0x2C, 0x00, 0x00, 0x00
  };

  patch(base, size, pat, sizeof(pat), 0x406A90 - 0x406AA5 /* negative on purpose */, skipDoubleInstanceCheck, 2, PatchType::Jmp);
}

void disableTitleScreenDemoMode(unsigned char* const base, size_t size) {
  // Disable countdown that sends title screen to demonstration mode
  // Write NOP NOP (0x90 0x90) at address 0x414AAF
  const DWORD targetOffset = 0x14AAF;  // 0x414AAF - base address
  unsigned char* patchAddress = base + targetOffset;
  
  char debugBuffer[256];
  sprintf_s(debugBuffer, "Disabling title screen demo countdown at 0x%p\n", (void*)(base + targetOffset));
  OutputDebugStringA(debugBuffer);
  
  // Change memory protection to allow writing
  DWORD oldProtect;
  if (!VirtualProtect(patchAddress, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
    OutputDebugStringA("Failed to change memory protection for title screen demo disable!\n");
    return;
  }
  
  // Write NOP NOP (0x90 0x90) to disable the countdown instructions
  patchAddress[0] = 0x90;  // NOP
  patchAddress[1] = 0x90;  // NOP
  
  // Restore original memory protection
  VirtualProtect(patchAddress, 2, oldProtect, &oldProtect);
  
  OutputDebugStringA("Title screen demo countdown successfully disabled!\n");
}

// I don't like this pattern. Unlike most patterns, it takes code
// from a previous function. I couldn't just use code from the original
// function because apparently there are two copies of it,
// one for zx player and one for as player. Go figure.
/* const unsigned char zxInputRoutinePattern[] = {
    0xe8, 0xd8, 0xe3, 0x01, 0x00,
    0x83, 0xc4, 0x28,
    0x33, 0xc0,
    0xc3,
    0xcc,
    0xcc,
    0x33, 0xc9,
    0x66, 0x39, 0x0d
}; */

/* // See comment in patchAsInputEnter regarding ensureAsInputFollowsZxInput
void ensureAsInputFollowsZxInput(unsigned char * const base, size_t baseSz) {
  const unsigned char * const patPtr = findPattern(base, baseSz, zxInputRoutinePattern,
						   sizeof(zxInputRoutinePattern) - 5 /* - 5 because pattern includes something that we already patched );
  if (!patPtr) {
    cryAndDie("Failed to detect zxInputRoutinePattern in ensureAsInputFollowsZxInput");
  }
  const unsigned char * const testPtr = patPtr + (0x4113EF - 0x411273);
  static const unsigned char testPat[] = {
    0x33, 0xC0, 0xF6, 0xC5, 0x80, 0x74, 0x05, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xF6, 0xC5, 0x40, 0x74,
    0x03, 0x83, 0xC8, 0x02, 0xF6, 0xC5, 0x20, 0x74, 0x03, 0x83, 0xC8, 0x04, 0xF6, 0xC5, 0x10, 0x74,
    0x03, 0x83, 0xC8, 0x08, 0xF6, 0xC1, 0x08, 0x74, 0x03, 0x83, 0xC8, 0x20, 0xF6, 0xC1, 0x04, 0x74,
    0x03, 0x83, 0xC8, 0x40, 0xF6, 0xC1, 0x02
  };
  for (size_t i = 0; i < sizeof(testPat); ++i) {
    if (testPtr[i] != testPat[i]) {
      cryAndDie("as input routine does not follow zx input routine");
    }
  }
} */

/* void patchZxInputRet(unsigned char* const base, size_t size) {
  const auto& pat = zxInputRoutinePattern;
  patch(base, size, pat, sizeof(pat), 0x411377 - 0x411273, zxInputRetHookWrapper, 0, PatchType::Jmp);
}

void patchZxInputEnter(unsigned char* const base, size_t size) {
  const auto& pat = zxInputRoutinePattern;

  unsigned char overriddenCode[64];
  patch(base, size, pat, sizeof(pat), 0x411280 - 0x411273, zxInputEnterHookWrapper, 4, PatchType::Call, overriddenCode);
  memcpy(&addressThatZxInputComparedAgainst, overriddenCode + 5, 4);
} */

/* void patchAsInputRet(unsigned char* const base, size_t size) {
  // This part is very ugly. Basically the as input routine is nearly identical to the zx input routine,
  // and starts 0x100 bytes after it.
  // Because of this, we just give a 0x100 offset here and call ensureAsInputFollowsZxInput to make sure
  // that as input routine is really where it should be.
  ensureAsInputFollowsZxInput(base, size);
  const auto& pat = zxInputRoutinePattern;
  patch(base, size, pat, sizeof(pat) - 5 /* - 5 because pattern includes something that we already patched 
	0x411477 - 0x411273, zxInputRetHookWrapper, 0, PatchType::Jmp);
}

void patchAsInputEnter(unsigned char* const base, size_t size) {
  ensureAsInputFollowsZxInput(base, size); // See comment in patchAsInputEnter regarding ensureAsInputFollowsZxInput
  const auto& pat = zxInputRoutinePattern;

  unsigned char overriddenCode[64];
  patch(base, size, pat, sizeof(pat), 0x411380 - 0x411273, asInputEnterHookWrapper, 4, PatchType::Call, overriddenCode);
  memcpy(&addressThatAsInputComparedAgainst, overriddenCode + 5, 4);
}
 */
void hookTimeStall(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + TIME_STALL_ADDRESS;
    DWORD timeStallHookAddr = (DWORD)timeStallHook;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking TimeStall @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)timeStallHookAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = timeStallHookAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for TimeStall hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("TimeStall function successfully replaced with new implementation!\n");
}



void addFrmSpriteToRenderBufferHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + addFrmSpriteToRenderBuffer_ADDRESS;
    DWORD addFrmSpriteToRenderBuffer_newAddr = (DWORD)addFrmSpriteToRenderBuffer_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking addFrmSpriteToRenderBuffer @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)addFrmSpriteToRenderBuffer_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = addFrmSpriteToRenderBuffer_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for addFrmSpriteToRenderBuffer hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("addFrmSpriteToRenderBuffer function successfully replaced with new implementation!\n");
}

void applyNetHost(bool use_local_ip, int delay, unsigned char* const base, size_t size) {
//  patchZxInputRet(base, size);
//  patchAsInputEnter(base, size);
  startRecorder();
  int port = startListening();
  std::string ip_addr = (use_local_ip ? std::string("127.0.0.1") : getExternalIp()) + ":" + std::to_string(port);
  std::string copied_to_clipboard = setClipboard(ip_addr) ? " (copied to clipboard)" : "";
  MessageBoxA(0, ip_addr.c_str(), ("Give this address to your friend" + copied_to_clipboard).c_str(), MB_OK);
  waitForClient(delay);
  MessageBoxA(0, "Host got client! Now what?", "Host", MB_OK);
}

void applyNetClient(const char* addrStr, int delay, unsigned char* const base, size_t size) {
//  patchZxInputEnter(base, size);
//  patchAsInputRet(base, size);
  startRecorder();
  const bool isTunnel = strlen(addrStr) > 3 && memcmp(addrStr, "127", 3) != 0; // TODO: always tunnel? should probably change
  connectToHost(addrStr, isTunnel, delay);
  MessageBoxA(0, "Client got host! Now what?", "Client", MB_OK);
}

// Structure to pass data to window enumeration callback
struct WindowEnumData {
    DWORD processId;
    HWND foundWindow;
    char windowClass[256];
    char windowTitle[256];
};

// Callback function for EnumWindows to find our process's main window
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    WindowEnumData* data = (WindowEnumData*)lParam;
    
    // Get the process ID that owns this window
    DWORD windowProcessId;
    GetWindowThreadProcessId(hwnd, &windowProcessId);
    
    // Check if this window belongs to our process
    if (windowProcessId == data->processId) {
        // Only consider visible windows
        if (IsWindowVisible(hwnd)) {
            char title[256] = {0};
            char className[256] = {0};
            
            GetWindowTextA(hwnd, title, sizeof(title));
            GetClassNameA(hwnd, className, sizeof(className));
            
            DebugOutput("ARGENTUM: Process window - Title: '%s', Class: '%s', HWND: %p\n", 
                       title, className, hwnd);
            
            // Filter out unwanted window types
            bool isConsoleWindow = (strcmp(className, "ConsoleWindowClass") == 0);
            bool isChildWindow = (GetParent(hwnd) != NULL);
            
            // Look for proper game windows - exclude console windows and child windows
            if (!isConsoleWindow && !isChildWindow) {
                // Prefer windows with game-like characteristics
                bool hasGameTitle = (strstr(title, "Moon") != NULL || 
                                   strstr(title, "ML") != NULL ||
                                   strlen(title) == 0);  // SDL windows often start without titles
                
                // Check for common game window classes, prioritizing SDL3
                bool isSDLWindow = (strstr(className, "SDL") != NULL);
                bool isGameWindowClass = (isSDLWindow ||
                                        strstr(className, "OpenGL") != NULL ||
                                        strstr(className, "DirectX") != NULL ||
                                        strstr(className, "Game") != NULL ||
                                        strcmp(className, "Window") == 0);
                
                // SDL windows get priority
                if (isSDLWindow) {
                    DebugOutput("ARGENTUM: Found SDL window! Class: '%s', Title: '%s'\n", className, title);
                }
                
                if (isGameWindowClass) {
                    data->foundWindow = hwnd;
                    strcpy_s(data->windowClass, className);
                    strcpy_s(data->windowTitle, title);
                    
                    DebugOutput("ARGENTUM: Selected game window - Title: '%s', Class: '%s', HWND: %p\n", 
                               title, className, hwnd);
                    
                    // If we found an SDL window, stop searching immediately
                    if (isSDLWindow) {
                        DebugOutput("ARGENTUM: SDL window found - stopping search\n");
                        return FALSE;
                    }
                    
                    // For other window types, keep looking for better candidates (like SDL)
                }
            } else {
                DebugOutput("ARGENTUM: Skipping window - Console: %s, Child: %s\n", 
                           isConsoleWindow ? "YES" : "NO", isChildWindow ? "YES" : "NO");
            }
        }
    }
    
    return TRUE; // Continue enumeration
}

// Fallback callback function for EnumWindows to find any non-console window
BOOL CALLBACK EnumWindowsProcFallback(HWND hwnd, LPARAM lParam) {
    WindowEnumData* data = (WindowEnumData*)lParam;
    DWORD windowProcessId;
    GetWindowThreadProcessId(hwnd, &windowProcessId);
    
    if (windowProcessId == data->processId && IsWindowVisible(hwnd)) {
        char className[256] = {0};
        GetClassNameA(hwnd, className, sizeof(className));
        
        // Accept any window that's NOT a console window
        if (strcmp(className, "ConsoleWindowClass") != 0 && GetParent(hwnd) == NULL) {
            data->foundWindow = hwnd;
            strcpy_s(data->windowClass, className);
            GetWindowTextA(hwnd, data->windowTitle, sizeof(data->windowTitle));
            
            DebugOutput("ARGENTUM: Fallback - Using window Class: '%s', Title: '%s'\n", 
                       className, data->windowTitle);
            return FALSE; // Stop enumeration
        }
    }
    return TRUE;
}

HWND findGameWindow() {
    DWORD currentProcessId = GetCurrentProcessId();
    
    WindowEnumData enumData = {0};
    enumData.processId = currentProcessId;
    enumData.foundWindow = NULL;
    
    DebugOutput("ARGENTUM: Searching for game window (Process ID: %d)...\n", currentProcessId);
    
    // Enumerate all top-level windows
    EnumWindows(EnumWindowsProc, (LPARAM)&enumData);
    
    if (enumData.foundWindow) {
        DebugOutput("ARGENTUM: Selected window - Title: '%s', Class: '%s'\n", 
                   enumData.windowTitle, enumData.windowClass);
    }
    
    return enumData.foundWindow;
}

void initializeArgentum() {
    DebugOutput("ARGENTUM: Starting argentum initialization...\n");
    
    // Wait for the game window to be created using a proper process-based approach
    HWND gameWindow = NULL;
    int attempts = 0;
    const int maxAttempts = 100; // Increased since SDL3 window creation might take longer
    
    while (!gameWindow && attempts < maxAttempts) {
        gameWindow = findGameWindow();
        
        if (!gameWindow) {
            if (attempts % 20 == 0) {  // Log every 4 seconds
                DebugOutput("ARGENTUM: Waiting for proper game window creation (attempt %d/%d)...\n", attempts + 1, maxAttempts);
                DebugOutput("ARGENTUM: Looking for SDL3/OpenGL/DirectX window, excluding console windows...\n");
            }
            Sleep(200);
            attempts++;
        }
    }
    
    // If we still don't have a window, try a fallback approach
    if (!gameWindow) {
        DebugOutput("ARGENTUM: No proper game window found, trying fallback methods...\n");
        
        // Fallback 1: Look for any non-console window from our process
        WindowEnumData enumData = {0};
        enumData.processId = GetCurrentProcessId();
        enumData.foundWindow = NULL;
        
        EnumWindows(EnumWindowsProcFallback, (LPARAM)&enumData);
        
        gameWindow = enumData.foundWindow;
    }
    
    if (!gameWindow) {
        DebugOutput("ARGENTUM: ERROR - No suitable window found after %d attempts!\n", attempts);
        DebugOutput("ARGENTUM: Proceeding without window handle - argentum will initialize when SDL3 window is created\n");
        // Don't return here - let argentum initialize anyway, it might work with the SDL3 window
    } else {
        DebugOutput("ARGENTUM: Game window found after %d attempts (HWND: %p)\n", attempts, gameWindow);
    }
    
    // Check if argentum engine is valid
    if (!argentum::g_engine) {
        DebugOutput("ARGENTUM: ERROR - g_engine is null!\n");
        MessageBoxA(NULL, "Argentum engine is null", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    DebugOutput("ARGENTUM: Skipping DirectX initialization - using SDL3 instead\n");
    // Skip DirectX initialization since we're using SDL3 for rendering
    // The argentum overlay will work without DirectX hooks
    
    DebugOutput("ARGENTUM: SDL3 rendering system active, argentum ready\n");
    
    // Check if argentum context is valid
    if (!argentum::g_ctx) {
        DebugOutput("ARGENTUM: ERROR - g_ctx is null!\n");
        MessageBoxA(NULL, "Argentum context is null", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    
    DebugOutput("ARGENTUM: Running argentum context...\n");
    // Remove callback - we'll handle controller config window in the menu.cpp file
    argentum::g_ctx->run();
    
    DebugOutput("ARGENTUM: Argentum context run completed\n");
}

void patchDisplayFontSprite(unsigned char* const base, size_t size) {
    // DebugOutput("Attempting to patch displayFontSprite\n");
    
    // The address we want to patch
    unsigned char* patchAddress = base + 0x14a9a;  // Known offset from base address



    // Directly patch the call to our custom function
    patch(base, size, 
          patchAddress, 5,  // Use the exact address and size of the CALL instruction
          0,  // No offset needed since we're targeting the exact address
          displayFontSpriteHookWrapper, 
          0,  // No NOPs needed
          PatchType::Call);

   // DebugOutput("Attempted to patch displayFontSprite\n");
}

/* void patchProcessJoystickInput(unsigned char* const base, size_t size) {
    DebugOutput("Attempting to patch ProcessJoystickInput\n");
    
    // The address we want to patch - call to ProcessJoystickInput at 0x0041129A
    unsigned char* patchAddress = base + 0x1129A;  // Address 0x0041129A relative to base
    
    // Verify this is a call instruction
    if (patchAddress[0] != 0xE8) {
        cryAndDie("Expected CALL instruction at ProcessJoystickInput location");
    }
    
    // Save the original function address for later use
    DWORD relativeOffset = *(DWORD*)(patchAddress + 1);
    DWORD targetAddress = (DWORD)(patchAddress + 5 + relativeOffset);
    originalProcessJoystickInput = (ProcessJoystickInputFunc)targetAddress;
    
    // Directly patch the call to our custom function
    patch(base, size, 
          patchAddress, 5,  // Use the exact address and size of the CALL instruction
          0,  // No offset needed since we're targeting the exact address
          processJoystickInputHookWrapper, 
          0,  // No NOPs needed
          PatchType::Call);

    DebugOutput("Successfully patched ProcessJoystickInput call at 0x%08X\n", 
                 (DWORD)patchAddress - (DWORD)base + (DWORD)GetModuleHandle(NULL));
} */

void hookBattleGameLoop(unsigned char* const base, size_t size) {
    // The BattleGameLoop function is at 0x41DEE0 - calculate the actual address
    DWORD gameBaseAddress = (DWORD)GetModuleHandle(NULL);
    DWORD offsetBattleGameLoop = 0x1DEE0; // Offset from base address
    unsigned char* originalFunction = (unsigned char*)(gameBaseAddress + offsetBattleGameLoop);
    unsigned char* hookFunction = (unsigned char*)BattleGameLoopHook;

    DebugOutput("hookBattleGameLoop: Original function at %p, hook at %p\n", originalFunction, hookFunction);

    // Calculate the relative jump address (target - source - 5)
    // Need to jump from originalFunction to hookFunction
    int relativeAddress = (int)(hookFunction - originalFunction - 5);
    DebugOutput("hookBattleGameLoop: Calculated relative jump address: 0x%X\n", relativeAddress);

    // Create a JMP instruction (E9 xx xx xx xx) to our hook
    unsigned char jmpInstruction[5] = { 0xE9, 0x00, 0x00, 0x00, 0x00 };
    memcpy(jmpInstruction + 1, &relativeAddress, sizeof(relativeAddress));

    // Change memory protection to allow writing
    DWORD oldProtect;
    BOOL protectResult = VirtualProtect(originalFunction, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    
    if (!protectResult) {
        DebugOutput("hookBattleGameLoop: ERROR - VirtualProtect failed with code %d\n", GetLastError());
        return;
    }
    
    DebugOutput("hookBattleGameLoop: Memory protection changed successfully\n");

    // Check first few bytes of the function to see what we're overwriting
    DebugOutput("hookBattleGameLoop: First 5 bytes before patching: %02X %02X %02X %02X %02X\n", 
        originalFunction[0], originalFunction[1], originalFunction[2], 
        originalFunction[3], originalFunction[4]);

    // Write the JMP instruction to the start of the original function
    memcpy(originalFunction, jmpInstruction, 5);
    
    // Verify the patch was applied
    DebugOutput("hookBattleGameLoop: First 5 bytes after patching: %02X %02X %02X %02X %02X\n", 
        originalFunction[0], originalFunction[1], originalFunction[2], 
        originalFunction[3], originalFunction[4]);

    // Restore the original memory protection
    VirtualProtect(originalFunction, 5, oldProtect, &oldProtect);
    
    DebugOutput("hookBattleGameLoop: Hook installed successfully at address 0x%p\n", originalFunction);
}

void installMainLoopHook() {
    HMODULE gameModule = GetModuleHandle(NULL);
    if (!gameModule) return;
    
    // Address of MainGameLoop_todo function
    DWORD mainGameLoopAddr = (DWORD)gameModule + 0x11030;  // 0x411030
    
    // Our hook function address
    DWORD hookAddr = (DWORD)&ProcessGameFrameHook;
    
    // Create jump instruction
    BYTE jumpCode[5] = { 0xE9, 0, 0, 0, 0 };  // E9 = JMP near relative
    *(DWORD*)(jumpCode + 1) = hookAddr - (mainGameLoopAddr + 5);
    
    // Write the jump code to the start of MainGameLoop_todo
    DWORD oldProtect;
    VirtualProtect((LPVOID)mainGameLoopAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((LPVOID)mainGameLoopAddr, jumpCode, 5);
    VirtualProtect((LPVOID)mainGameLoopAddr, 5, oldProtect, &oldProtect);
    
    DebugOutput("MainGameLoop_todo function successfully hooked\n");
}


void clearGlobalAnimControlHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + OFFSET_CLEARGLOBALANIMCONTROL;
    DWORD clearGlobalAnimControl_newAddr = (DWORD)ClearGlobalAnimControl_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking ClearGlobalAnimControl @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)clearGlobalAnimControl_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = clearGlobalAnimControl_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for ClearGlobalAnimControl hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("ClearGlobalAnimControl function successfully replaced with new implementation!\n");
}

void updateRenderStateHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + updateRenderState_ADDRESS;
    DWORD updateRenderState_newAddr = (DWORD)updateRenderState_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking updateRenderState @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)updateRenderState_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = updateRenderState_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for updateRenderState hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("updateRenderState function successfully replaced with new implementation!\n");
}

void UpdatePointerArrayHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + UpdatePointerArray_ADDRESS;
    DWORD UpdatePointerArray_newAddr = (DWORD)UpdatePointerArray_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking UpdatePointerArray @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)UpdatePointerArray_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = UpdatePointerArray_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for UpdatePointerArray hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("UpdatePointerArray function successfully replaced with new implementation!\n");
}

void resetResourceCounterHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + resetResourceCounter_ADDRESS;
    DWORD resetResourceCounter_newAddr = (DWORD)resetResourceCounter_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking resetResourceCounter @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)resetResourceCounter_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = resetResourceCounter_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for resetResourceCounter hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("resetResourceCounter function successfully replaced with new implementation!\n");
}

void ResetGameVariable_todoHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + ResetGameVariable_todo_ADDRESS;
    DWORD ResetGameVariable_todo_newAddr = (DWORD)ResetGameVariable_todo_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking ResetGameVariable_todo @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)ResetGameVariable_todo_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = ResetGameVariable_todo_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for ResetGameVariable_todo hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("ResetGameVariable_todo function successfully replaced with new implementation!\n");
}

void cleanupResourcesHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + cleanupResources_ADDRESS;
    DWORD cleanupResources_newAddr = (DWORD)cleanupResources_new;

    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking cleanupResources @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)cleanupResources_newAddr);
    OutputDebugStringA(debugBuffer);

    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = cleanupResources_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for cleanupResources hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("cleanupResources function successfully replaced with new implementation!\n");
}

#define INTERNAL_FRM_SPRITE_ADDRESS 0x2F650   // 0x42F650 - InternalFrmSprite

// Hook function for InternalFrmSprite
void InternalFrmSpriteHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + INTERNAL_FRM_SPRITE_ADDRESS; 
    DWORD newFuncAddr = (DWORD)InternalFrmSprite_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking InternalFrmSprite @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)newFuncAddr);
    OutputDebugStringA(debugBuffer);
    DebugOutput("InternalFrmSprite: Original at 0x%p, new at 0x%p\n", (void*)originalFuncAddr, (void*)newFuncAddr);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = newFuncAddr - originalFuncAddr - 5;
    
    // Read original bytes for verification
    unsigned char originalBytes[5];
    memcpy(originalBytes, (void*)originalFuncAddr, 5);
    DebugOutput("InternalFrmSprite: Original bytes: %02X %02X %02X %02X %02X\n", 
        originalBytes[0], originalBytes[1], originalBytes[2], 
        originalBytes[3], originalBytes[4]);
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DWORD error = GetLastError();
        OutputDebugStringA("Failed to change memory protection for InternalFrmSprite hook!\n");
        DebugOutput("InternalFrmSprite: VirtualProtect failed with error %d\n", error);
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Verify the patch
    unsigned char patchedBytes[5];
    memcpy(patchedBytes, (void*)originalFuncAddr, 5);
    DebugOutput("InternalFrmSprite: Patched bytes: %02X %02X %02X %02X %02X\n", 
        patchedBytes[0], patchedBytes[1], patchedBytes[2], 
        patchedBytes[3], patchedBytes[4]);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("InternalFrmSprite function successfully replaced with new implementation!\n");
}

void ReallocateRenderBufferHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DebugOutput("=== SETUP: Setting up ReallocateRenderBuffer hook ===\n");
    
    // Log detailed base address information
    DebugOutput("Base address: 0x%p\n", (void*)baseAddr);
    DebugOutput("Module size: 0x%zX bytes\n", moduleSize);
    
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + ReallocateRenderBuffer_ADDRESS;
    DWORD ReallocateRenderBuffer_newAddr = (DWORD)ReallocateRenderBuffer_new;

    DebugOutput("Hook details:\n");
    DebugOutput(" - ReallocateRenderBuffer_ADDRESS: 0x%X\n", ReallocateRenderBuffer_ADDRESS);
    DebugOutput(" - Original function at: 0x%p\n", (void*)originalFuncAddr);
    DebugOutput(" - Our implementation at: 0x%p\n", (void*)ReallocateRenderBuffer_newAddr);
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking ReallocateRenderBuffer @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)ReallocateRenderBuffer_newAddr);
    OutputDebugStringA(debugBuffer);

    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = ReallocateRenderBuffer_newAddr - originalFuncAddr - 5;
    DebugOutput(" - Relative jump offset: 0x%X\n", relativeJumpOffset);
    
    // Read original bytes for verification
    unsigned char originalBytes[5];
    memcpy(originalBytes, (void*)originalFuncAddr, 5);
    DebugOutput(" - Original bytes: %02X %02X %02X %02X %02X\n", 
        originalBytes[0], originalBytes[1], originalBytes[2], 
        originalBytes[3], originalBytes[4]);
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DWORD error = GetLastError();
        DebugOutput("ERROR: Failed to change memory protection! Error code: %d\n", error);
        OutputDebugStringA("Failed to change memory protection for ReallocateRenderBuffer hook!\n");
        return;
    }
    
    DebugOutput(" - Memory protection changed (old protection: 0x%X)\n", oldProtect);
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Verify the hook was installed correctly
    unsigned char verifyBytes[5];
    memcpy(verifyBytes, (void*)originalFuncAddr, 5);
    DebugOutput(" - Installed bytes: %02X %02X %02X %02X %02X\n", 
        verifyBytes[0], verifyBytes[1], verifyBytes[2], 
        verifyBytes[3], verifyBytes[4]);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    DebugOutput(" - Memory protection restored\n");
    
    OutputDebugStringA("ReallocateRenderBuffer function successfully replaced with new implementation!\n");
    DebugOutput("=== SETUP: ReallocateRenderBuffer hook completed ===\n");
}

void onAttach() {
  DebugOutput("Starting onAttach initialization\n");
  DebugOutput("=== ENABLED SYSTEMS AFTER INITGAME TAKEOVER ===\n");
  DebugOutput("? SDL3 Graphics System (DirectDraw replacement)\n");
  DebugOutput("? SDL3 Input System (Simplified hooks)\n");
  DebugOutput("? ImGui Rendering (Argentum overlay)\n");
  DebugOutput("? Core Sprite Rendering Hooks\n");
  DebugOutput("? Main Loop Hooks (Required for ImGui)\n");
  DebugOutput("? Battle System Hooks\n");
  DebugOutput("? VSE Data Processing\n");
  DebugOutput("? Palette Management System\n");
  DebugOutput("===============================================\n");
  
  TCHAR exeName[MAX_PATH + 1];
  GetModuleFileName(NULL, exeName, MAX_PATH + 1);
  DebugOutput("Executable name: %s\n", exeName);

  MODULEINFO modinfo = {0};
  HMODULE module = GetModuleHandle(exeName);
  if (module == 0) cryAndDie("GetModuleHandle failed");
  GetModuleInformation(GetCurrentProcess(), module, &modinfo, sizeof(MODULEINFO));

  uintptr_t baseAddr = (uintptr_t)modinfo.lpBaseOfDll;
  const auto moduleSize = modinfo.SizeOfImage;
  DebugOutput("Module base address: 0x%p, size: 0x%zX\n", (void*)baseAddr, moduleSize);
  
  DebugOutput("Installing memory hooks...\n");
   // installMemoryHooks(); // TEMPORARILY DISABLED FOR DEBUGGING

  // Initialize caster libraries
  char buf[512];
  DebugOutput("Initializing caster libraries...\n");
  Logger::get().initialize(GetEnvironmentVariableA("MOONLIGHT_CASTER_NET_LOCAL_HOST", buf, sizeof(buf)) ? "ml_hook_host_log" : "ml_hook_log");
  TimerManager::get().initialize();
  SocketManager::get().initialize();

  DebugOutput("Setting up core sprite rendering hooks...\n");
  InternalFrmSpriteHOOK(baseAddr, moduleSize);
  clearGlobalAnimControlHOOK(baseAddr, moduleSize);
  updateRenderStateHOOK(baseAddr, moduleSize);
  UpdatePointerArrayHOOK(baseAddr, moduleSize);
  resetResourceCounterHOOK(baseAddr, moduleSize);
  ResetGameVariable_todoHOOK(baseAddr, moduleSize);
  cleanupResourcesHOOK(baseAddr, moduleSize);
  ReallocateRenderBufferHOOK(baseAddr, moduleSize);
  addFrmSpriteToRenderBufferHOOK(baseAddr, moduleSize);
  DebugOutput("Setting up resource array hook...\n");
  ReallocateGlobalResourceArrayHOOK(baseAddr, moduleSize);
  
  DebugOutput("Setting up time stall hook...\n");
  hookTimeStall(baseAddr, moduleSize);
  
  DebugOutput("Setting up CreateMainWindow to SDL3 hook...\n");
  CreateMainWindowHOOK(baseAddr, moduleSize);
  
  DebugOutput("Setting up DirectDraw to SDL3 hook...\n");
  initDirectDrawHOOK(baseAddr, moduleSize);
  
  DebugOutput("Setting up UpdateColorInformation hook...\n");
  UpdateColorInformationHOOK(baseAddr, moduleSize);
  
  DebugOutput("Setting up UpdatePaletteEntries hook to fix boot splash palette...\n");
  UpdatePaletteEntriesHOOK(baseAddr, moduleSize);
  
  // Timer-based approach disabled - now using proper UpdatePaletteEntries hook
  DebugOutput("Automatic palette correction timer disabled - using hook-based approach instead\n");

  
  DebugOutput("Setting up InitializeResourceHandlers hook...\n");
  InitializeResourceHandlersHOOK(baseAddr, moduleSize);
  
  DebugOutput("Setting up ProcessScreenUpdatesAndResources hook...\n");
  ProcessScreenUpdatesAndResourcesHOOK(baseAddr, moduleSize);
  
  DebugOutput("Setting up additional SDL3-compatible hooks...\n");
  InitializeWindowHOOK(baseAddr, moduleSize);
  isGraphicsSystemInitializedHOOK(baseAddr, moduleSize);
  
  DebugOutput("Initializing MinHook for palette hooks...\n");
  MH_STATUS mh_status = MH_Initialize();
  if (mh_status != MH_OK && mh_status != MH_ERROR_ALREADY_INITIALIZED) {
    DebugOutput("ERROR: Failed to initialize MinHook! Status: %d\n", mh_status);
  } else {
    if (mh_status == MH_ERROR_ALREADY_INITIALIZED) {
      DebugOutput("MinHook already initialized (expected)\n");
    } else {
      DebugOutput("MinHook initialized successfully\n");
    }
    
    DebugOutput("Installing palette hooks...\n");
    DWORD getPaletteEntryAddr = baseAddr + GET_PALETTE_ENTRY_ADDRESS;
    g_getPaletteEntryAddr = getPaletteEntryAddr;  // Store for cleanup
    if (!argentum::hooks::InstallPaletteHooks(getPaletteEntryAddr)) {
      DebugOutput("WARNING: Failed to install palette hooks!\n");
      g_getPaletteEntryAddr = 0;  // Reset on failure
    } else {
      DebugOutput("Palette hooks installed successfully!\n");
    }
  }
  
  DebugOutput("Setting up main loop hook...\n");
  installMainLoopHook();
  
  DebugOutput("Setting up initGame replacement hook...\n");
  if (!InstallInitGameHook()) {
    DebugOutput("ERROR: Failed to install initGame hook!\n");
    cryAndDie("Failed to install initGame hook");
  } else {
    DebugOutput("InitGame hook installed successfully!\n");
  }
  
  DebugOutput("Setting up joystick hook...\n");
  // setupJoystickHook(module);
  // patchProcessJoystickInput((unsigned char*)baseAddr, moduleSize);
  
  DebugOutput("Setting up VSE data processing hooks...\n");
  processVSEDataHOOK(baseAddr, moduleSize);
  processVSEEntryHOOK(baseAddr, moduleSize);
  
  DebugOutput("Skipping fullscreen crash fix - using SDL3 rendering pipeline...\n");
  // applyFullScreenCrashFix((unsigned char*)baseAddr, moduleSize); // Not needed with SDL3
  DebugOutput("Setting up battle game loop hook...\n");
  hookBattleGameLoop((unsigned char*)baseAddr, moduleSize);

  DebugOutput("Setting up other hooks...\n");
 // patchDisplayFontSprite((unsigned char*)baseAddr, moduleSize);
  applyAllowDoubleInstance((unsigned char*)baseAddr, moduleSize);
  disableTitleScreenDemoMode((unsigned char*)baseAddr, moduleSize);
  initFunctionPointers(GetModuleHandle(NULL));
  
  DebugOutput("Setting up simplified input hooks (replacing complex assembly wrappers)...\n");
  if (!installSimplifiedInputHooks()) {
    DebugOutput("ERROR: Failed to install simplified input hooks!\n");
    cryAndDie("Failed to install simplified input hooks");
  }
  
  DebugOutput("Setting up netplay or replay mode...\n");
  if (GetEnvironmentVariableA("MOONLIGHT_CASTER_REPLAY", buf, sizeof(buf))) {
    DebugOutput("Replay mode activated\n");
    startReplayer(buf);
  } else if (GetEnvironmentVariableA("MOONLIGHT_CASTER_NET_SERVER", buf, sizeof(buf)) ||
	     GetEnvironmentVariableA("MOONLIGHT_CASTER_NET_LOCAL_HOST", buf, sizeof(buf))) {
    DebugOutput("Net host mode activated\n");
    startRecorder();
    int port = startListening();
    std::string ip_addr = (GetEnvironmentVariableA("MOONLIGHT_CASTER_NET_LOCAL_HOST", buf, sizeof(buf)) ? std::string("127.0.0.1") : getExternalIp()) + ":" + std::to_string(port);
    std::string copied_to_clipboard = setClipboard(ip_addr) ? " (copied to clipboard)" : "";
    MessageBoxA(0, ip_addr.c_str(), ("Give this address to your friend" + copied_to_clipboard).c_str(), MB_OK);
    waitForClient(readDelay());
    MessageBoxA(0, "Host got client! Now what?", "Host", MB_OK);
  } else if (GetEnvironmentVariableA("MOONLIGHT_CASTER_NET_CLIENT", buf, sizeof(buf))) {
    DebugOutput("Net client mode activated\n");
    startRecorder();
    const bool isTunnel = strlen(buf) > 3 && memcmp(buf, "127", 3) != 0;
    connectToHost(buf, isTunnel, readDelay());
    MessageBoxA(0, "Client got host! Now what?", "Client", MB_OK);
  } else {
    DebugOutput("Normal recording mode activated\n");
    startRecorder();
  }

  DebugOutput("onAttach initialization completed successfully\n");
}



// Add this declaration at the top of the file
extern int __stdcall CtxDllMain(_In_ HINSTANCE instance, _In_ DWORD reason, _In_ LPVOID reserved);

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);
        try {
            onAttach();
            // Initialize Argentum (ImGui system) after hooks are set up
            std::thread([]() {
                // Wait for the game's basic systems to initialize first
                DebugOutput("ARGENTUM: Waiting for game systems to initialize before starting argentum...\n");
                Sleep(2000);  // Give the game 2 seconds to create its window systems
                initializeArgentum();
             }).detach();
        } catch (const std::exception& e) {
            cryAndDie(e.what());
        }
        break;
    }
    case DLL_PROCESS_DETACH: {
        // Cleanup resources
        try {
            // Uninstall palette hooks
            if (g_getPaletteEntryAddr != 0) {
                argentum::hooks::UninstallPaletteHooks(g_getPaletteEntryAddr);
            }
            
            // DirectDraw to SDL3 hook cleanup is handled by MinHook shutdown
            
            // Clean up any open handles
            CloseHandle(GetCurrentProcess());
            
            // Flush any pending writes
            FlushFileBuffers(GetStdHandle(STD_OUTPUT_HANDLE));
            FlushFileBuffers(GetStdHandle(STD_ERROR_HANDLE));
            
            // Force unload DLL resources
            FreeLibrary(hModule);
        } catch (...) {
            // Ignore cleanup errors
        }
        break;
    }
    }

    return TRUE;
}

void ReallocateGlobalResourceArrayHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DebugOutput("=== SETUP: Setting up ReallocateGlobalResourceArray hook ===\n");
    
    // Log detailed base address information
    DebugOutput("Base address: 0x%p\n", (void*)baseAddr);
    DebugOutput("Module size: 0x%zX bytes\n", moduleSize);
    
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + ReallocateGlobalResourceArray_ADDRESS;
    DWORD ReallocateGlobalResourceArray_newAddr = (DWORD)ReallocateGlobalResourceArray_new;

    DebugOutput("Hook details:\n");
    DebugOutput(" - ReallocateGlobalResourceArray_ADDRESS: 0x%X\n", ReallocateGlobalResourceArray_ADDRESS);
    DebugOutput(" - Original function at: 0x%p\n", (void*)originalFuncAddr);
    DebugOutput(" - Our implementation at: 0x%p\n", (void*)ReallocateGlobalResourceArray_newAddr);
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking ReallocateGlobalResourceArray @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)ReallocateGlobalResourceArray_newAddr);
    OutputDebugStringA(debugBuffer);

    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = ReallocateGlobalResourceArray_newAddr - originalFuncAddr - 5;
    DebugOutput(" - Relative jump offset: 0x%X\n", relativeJumpOffset);
    
    // Read original bytes for verification
    unsigned char originalBytes[5];
    memcpy(originalBytes, (void*)originalFuncAddr, 5);
    DebugOutput(" - Original bytes: %02X %02X %02X %02X %02X\n", 
        originalBytes[0], originalBytes[1], originalBytes[2], 
        originalBytes[3], originalBytes[4]);
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DWORD error = GetLastError();
        DebugOutput("ERROR: Failed to change memory protection! Error code: %d\n", error);
        OutputDebugStringA("Failed to change memory protection for ReallocateGlobalResourceArray hook!\n");
        return;
    }
    
    DebugOutput(" - Memory protection changed (old protection: 0x%X)\n", oldProtect);
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Verify the hook was installed correctly
    unsigned char verifyBytes[5];
    memcpy(verifyBytes, (void*)originalFuncAddr, 5);
    DebugOutput(" - Installed bytes: %02X %02X %02X %02X %02X\n", 
        verifyBytes[0], verifyBytes[1], verifyBytes[2], 
        verifyBytes[3], verifyBytes[4]);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    DebugOutput(" - Memory protection restored\n");
    
    OutputDebugStringA("ReallocateGlobalResourceArray function successfully replaced with new implementation!\n");
    DebugOutput("=== SETUP: ReallocateGlobalResourceArray hook completed ===\n");
}

void processVSEDataHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + PROCESS_VSE_DATA_ADDRESS;
    DWORD newFuncAddr = (DWORD)processVSEDataHook;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking processVSEData @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)newFuncAddr);
    OutputDebugStringA(debugBuffer);

    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = newFuncAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for processVSEData hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("processVSEData function successfully replaced with new implementation!\n");
}

void processVSEEntryHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + PROCESS_VSE_ENTRY_ADDRESS;
    DWORD newFuncAddr = (DWORD)processVSEentry_new;  // Use our replicated function
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking processVSEEntry @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)newFuncAddr);
    OutputDebugStringA(debugBuffer);

    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = newFuncAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for processVSEEntry hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("processVSEEntry function successfully replaced with new implementation!\n");
}

void initDirectDrawHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + INIT_DIRECTDRAW_ADDRESS;
    DWORD initDirectDraw_newAddr = (DWORD)argentum::hooks::initDirectDraw_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking initDirectDraw @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)initDirectDraw_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = initDirectDraw_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for initDirectDraw hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("initDirectDraw function successfully replaced with SDL3 implementation!\n");
}

void CreateMainWindowHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + CREATE_MAIN_WINDOW_ADDRESS;
    DWORD CreateMainWindow_newAddr = (DWORD)argentum::hooks::CreateMainWindow_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking CreateMainWindow @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)CreateMainWindow_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = CreateMainWindow_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for CreateMainWindow hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("CreateMainWindow function successfully replaced with SDL3 implementation!\n");
}

void UpdateColorInformationHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + UPDATE_COLOR_INFORMATION_ADDRESS;
    DWORD UpdateColorInformation_newAddr = (DWORD)argentum::hooks::UpdateColorInformation_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking UpdateColorInformation @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)UpdateColorInformation_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = UpdateColorInformation_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for UpdateColorInformation hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("UpdateColorInformation function successfully replaced with SDL3 implementation!\n");
}

void InitializeResourceHandlersHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + INITIALIZE_RESOURCE_HANDLERS_ADDRESS;
    DWORD initializeResourceHandlers_newAddr = (DWORD)argentum::hooks::initializeResourceHandlers_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking initializeResourceHandlers @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)initializeResourceHandlers_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = initializeResourceHandlers_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for initializeResourceHandlers hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("initializeResourceHandlers function successfully replaced with SDL3 implementation!\n");
}

void ProcessScreenUpdatesAndResourcesHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + PROCESS_SCREEN_UPDATES_ADDRESS;
    DWORD processScreenUpdatesAddr = (DWORD)argentum::hooks::ProcessScreenUpdatesAndResources_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking ProcessScreenUpdatesAndResources @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)processScreenUpdatesAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = processScreenUpdatesAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for ProcessScreenUpdatesAndResources hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
          OutputDebugStringA("ProcessScreenUpdatesAndResources function successfully replaced with SDL3 implementation!\n");
  }

void InitializeWindowHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + INITIALIZE_WINDOW_ADDRESS;
    DWORD InitializeWindow_newAddr = (DWORD)argentum::hooks::InitializeWindow_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking InitializeWindow @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)InitializeWindow_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = InitializeWindow_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for InitializeWindow hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("InitializeWindow function successfully replaced with SDL3 implementation!\n");
}

void isGraphicsSystemInitializedHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + IS_GRAPHICS_INITIALIZED_ADDRESS;
    DWORD isGraphicsSystemInitialized_newAddr = (DWORD)argentum::hooks::isGraphicsSystemInitialized_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking isGraphicsSystemInitialized @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)isGraphicsSystemInitialized_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = isGraphicsSystemInitialized_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for isGraphicsSystemInitialized hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("isGraphicsSystemInitialized function successfully replaced with SDL3 implementation!\n");
}

void UpdatePaletteEntriesHOOK(uintptr_t baseAddr, size_t moduleSize) {
    DWORD jumpNearOpcode = 0xE9;
    DWORD originalFuncAddr = baseAddr + UPDATE_PALETTE_ENTRIES_ADDRESS;
    DWORD UpdatePaletteEntries_newAddr = (DWORD)argentum::hooks::UpdatePaletteEntries_new;
    
    char debugBuffer[256];
    sprintf_s(debugBuffer, "Hooking UpdatePaletteEntries @ 0x%p to 0x%p\n", (void*)originalFuncAddr, (void*)UpdatePaletteEntries_newAddr);
    OutputDebugStringA(debugBuffer);
    
    // Calculate the relative jump offset: destination - source - 5 (5 for the size of the jmp instruction)
    DWORD relativeJumpOffset = UpdatePaletteEntries_newAddr - originalFuncAddr - 5;
    
    // Apply the patch with proper error checking
    DWORD oldProtect;
    if (!VirtualProtect((void*)originalFuncAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        OutputDebugStringA("Failed to change memory protection for UpdatePaletteEntries hook!\n");
        return;
    }
    
    // Write the jump opcode
    memcpy((void*)originalFuncAddr, &jumpNearOpcode, 1);
    
    // Write the relative jump offset
    memcpy((void*)(originalFuncAddr + 1), &relativeJumpOffset, 4);
    
    // Restore protection
    VirtualProtect((void*)originalFuncAddr, 5, oldProtect, &oldProtect);
    
    OutputDebugStringA("UpdatePaletteEntries function successfully replaced with boot splash palette fix!\n");
}








