#include <stdexcept>  // Add this to resolve 'runtime_error' error
#include <windows.h> 
#include "../../argentum.hpp"
#include "../../third_party/include/minhook/minhook.h"
#include "../../hooks/hooks.hpp"
#include "../../hooks/impl/sdl3_context.hpp"  // Add SDL3 context access

// Add ImGui SDL3 backend includes
#include "../../third_party/include/imgui/imgui.h"
#include "../../include/imgui/imgui_impl_sdl3.h"
#include "../../third_party/include/imgui/imgui_impl_sdlrenderer3.h"

/* int __stdcall CtxDllMain(_In_ HINSTANCE instance, _In_ DWORD reason, _In_ LPVOID reserved) {
	if (reason != DLL_PROCESS_ATTACH)
		return 0;

	DisableThreadLibraryCalls(instance);
	std::thread{ []() {
		argentum::g_ctx->run(); }
	}.detach();
	return 1;
} */

#ifdef _DEBUG
#define THROW( exception ) throw std::runtime_error{ exception };
#else
#define THROW( exception ) return
#endif

#define HOOK( target, hook, original ) \
    if ( MH_CreateHook( reinterpret_cast<LPVOID>(target), \
        reinterpret_cast< LPVOID >( &hook ), reinterpret_cast< LPVOID* >( &original ) ) != MH_OK ) \
        THROW( "can't hook " #hook "." )

#define HOOK_VFUNC( vft, index, hook, original ) \
    if ( MH_CreateHook(  reinterpret_cast<LPVOID>(vft[index]), \
        reinterpret_cast< LPVOID >( &hook ), reinterpret_cast< LPVOID* >( &original ) ) != MH_OK ) \
        THROW( "Can't hook " #hook "." )

namespace argentum {
	void c_ctx::run() {
		// Modern SDL3-based initialization
		DebugOutput("argentum CTX: Initializing with pure SDL3 backend\n");
		
		MH_STATUS mh_status = MH_Initialize();
		if (mh_status != MH_OK && mh_status != MH_ERROR_ALREADY_INITIALIZED) {
			DebugOutput("argentum CTX: ERROR - Can't initialize MinHook (status: %d)\n", mh_status);
			THROW("Can't initialize MinHook");
		} else if (mh_status == MH_ERROR_ALREADY_INITIALIZED) {
			DebugOutput("argentum CTX: MinHook already initialized (expected)\n");
		} else {
			DebugOutput("argentum CTX: MinHook initialized successfully\n");
		}

		// Initialize SDL3-based hooks (ImGui is initialized automatically in rendering loop)
		init_hooks_sdl3();

		if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
			DebugOutput("argentum CTX: ERROR - Problem enabling hooks\n");
			THROW("There was a problem enabling some hook.");
		}
		
		DebugOutput("argentum CTX: Successfully initialized with pure SDL3 + ImGui\n");
	}

	void c_ctx::init_hooks() const {
		// Legacy DirectX hooks - no longer used
		DebugOutput("argentum CTX: WARNING - init_hooks() called but DirectX is deprecated\n");
	}

	void c_ctx::init_hooks_sdl3() const {

		// SDL3-based hook initialization (no DirectX)
		DebugOutput("argentum CTX: Initializing SDL3-based hooks\n");
		
		// Only hook keyboard functions that are independent of rendering system
		uint32_t scan_result = find_pattern("\x8D\x45\xA0\x50\x8B\x4D\x88\x8B\x11\x8B\x45\x88\x50\xFF\x92\x00\x00\x00\x00\xDB\xE2\x89\x45\x84\x83\x7D\x84\x00", "xxxxxxxxxxxxxxx????xxxxxxxxx");
		if (scan_result) {
			HOOK(scan_result - 0x8F, hooks::key_up, hooks::o_key_up);
			DebugOutput("argentum CTX: Successfully hooked keyboard functions\n");
		} else {
			DebugOutput("argentum CTX: WARNING - Could not find keyboard hook pattern\n");
		}
	}

	void c_ctx::init_imgui_sdl3() const {
		// Wait for SDL3 context to be initialized by the game
		int attempts = 0;
		while (!hooks::g_sdlContext.initialized && attempts < 100) {
			Sleep(10);  // Wait 10ms
			attempts++;
		}
		
		if (!hooks::g_sdlContext.initialized) {
			DebugOutput("argentum CTX: ERROR - SDL3 context not initialized, cannot setup ImGui\n");
			return;
		}

		DebugOutput("argentum CTX: Initializing ImGui with SDL3 backend\n");
		
		// Initialize ImGui
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
		
		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		
		// Initialize SDL3 + SDL Renderer backends
		if (!ImGui_ImplSDL3_InitForSDLRenderer(hooks::g_sdlContext.window, hooks::g_sdlContext.renderer)) {
			DebugOutput("argentum CTX: ERROR - Failed to initialize ImGui SDL3 backend\n");
			return;
		}
		
		if (!ImGui_ImplSDLRenderer3_Init(hooks::g_sdlContext.renderer)) {
			DebugOutput("argentum CTX: ERROR - Failed to initialize ImGui SDL Renderer backend\n");
			ImGui_ImplSDL3_Shutdown();
			return;
		}
		
		DebugOutput("argentum CTX: ImGui SDL3 backend initialized successfully\n");
	}

	void c_ctx::cleanup_imgui_sdl3() const {
		DebugOutput("argentum CTX: Cleaning up ImGui SDL3 backend\n");
		
		ImGui_ImplSDLRenderer3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();
	}
}

//#undef HOOK_VFUNC
//#undef THROW


