// locale_spoof_internal.h -- the two symbols that cross the
// locale_spoof.cpp / locale_spoof_dialog.cpp boundary.
//
// Everything else in the locale spoof keeps internal linkage inside its own
// TU's anonymous namespace. Only the DialogBoxParamA pair is shared, because
// InstallLocaleSpoof (locale_spoof.cpp) registers a detour that is defined in
// locale_spoof_dialog.cpp.
//
// The trampoline's type is spelled out longhand on purpose: locale_spoof.cpp
// has a `DialogBoxParamA_t` typedef inside its anonymous namespace, and those
// members remain visible at the enclosing global scope -- a same-named global
// typedef here would be ambiguous.
#pragma once

#include <windows.h>

// NOTE: kSpoofedCodePage is deliberately NOT declared here. locale_spoof.cpp
// defines it inside its anonymous namespace, whose members stay visible at the
// enclosing global scope -- a same-named constant here would be ambiguous in
// that TU. Each TU keeps its own copy of the constant instead.

// Defined in locale_spoof_dialog.cpp, bound by InstallLocaleSpoof.
extern INT_PTR (WINAPI* p_DialogBoxParamA)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);

// Detour: pulls the RT_DIALOG resource, re-encodes packed-SJIS strings via
// CP932, and hands a translated template to DialogBoxIndirectParamW.
INT_PTR WINAPI Hook_DialogBoxParamA(HINSTANCE inst, LPCSTR template_name,
                                    HWND parent, DLGPROC dlg_proc,
                                    LPARAM init_param);
