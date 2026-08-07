// locale_spoof_dialog.cpp -- DialogBoxParamA template translation, split
// verbatim out of locale_spoof.cpp (which had crept over the 1000-line limit).
//
// Pure move. The only linkage change is that p_DialogBoxParamA and
// Hook_DialogBoxParamA now have external linkage (declared in
// locale_spoof_internal.h) because InstallLocaleSpoof, which registers the
// hook, stayed behind in locale_spoof.cpp. The template-walking helpers keep
// internal linkage inside this TU's own anonymous namespace.

#include "locale_spoof.h"
#include "locale_spoof_internal.h"

#include <SDL3/SDL_log.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Same value as locale_spoof.cpp's copy. Kept per-TU rather than shared via
// the internal header: that file defines it inside an anonymous namespace
// whose members remain visible at global scope, so a header-level constant of
// the same name would be ambiguous there.
constexpr UINT kSpoofedCodePage = 932;   // Shift-JIS / CP932

// The DialogBoxParamA trampoline lives here (this TU is its only caller);
// InstallLocaleSpoof in locale_spoof.cpp binds it through the extern in
// locale_spoof_internal.h.
INT_PTR (WINAPI* p_DialogBoxParamA)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM) = nullptr;

// --- DialogBoxParamA: translate the dialog template resource ---
//
// Symptom: WW's "specify title" popup renders as `?????`. Cause: the
// dialog template is compiled into the .rc as wide UTF-16, but on a JP-
// system build many wide values are actually SJIS BYTES stuffed into the
// low half of WCHAR slots -- i.e. the resource compiler emitted SJIS bytes
// expecting the OS to interpret them via CP_ACP=932, which doesn't
// happen on US Windows. Our user-mode GetACP hook does NOT affect the
// kernel-side resource string conversion, so static labels render as `?`.
//
// Fix: hook DialogBoxParamA. Pull the RT_DIALOG resource ourselves, walk
// the DLGTEMPLATE (or DLGTEMPLATEEX) structure, and for every embedded
// wide-string field check if it looks like packed-SJIS-in-WCHAR. If so,
// flatten back to bytes, decode via CP932, write out a real wide string.
// Then call DialogBoxIndirectParamW with the rewritten template.
//
// Heuristic for "packed-SJIS-in-WCHAR": every WCHAR has its high byte
// zero (high-half ASCII range). True UTF-16 JP would have high bytes
// like 0x30 (Hiragana), 0x4E-0x9F (CJK), etc. Pure-ASCII titles also
// have all-zero high bytes -- for those, the round-trip via CP932 is a
// no-op, so always-translate is safe.
namespace {

// Reinterpret a sequence of WCHARs (each holding a single SJIS byte in low
// 8 bits) as a contiguous SJIS byte buffer, then decode via CP932 to real
// UTF-16. Returns the decoded wide string. If any WCHAR has a non-zero
// high byte, the string is treated as already-real-UTF-16 and copied as-is.
std::wstring DialogStringTranslate(const wchar_t* in, size_t in_len) {
    if (!in || in_len == 0) return {};
    bool packed_sjis = true;
    for (size_t i = 0; i < in_len; ++i) {
        if ((unsigned)(in[i] >> 8) != 0) { packed_sjis = false; break; }
    }
    if (!packed_sjis) return std::wstring(in, in_len);
    // Flatten low bytes
    std::vector<char> sjis(in_len);
    for (size_t i = 0; i < in_len; ++i) sjis[i] = (char)(in[i] & 0xFF);
    // Decode via CP932
    int wlen = MultiByteToWideChar(kSpoofedCodePage, 0,
                                   sjis.data(), (int)sjis.size(),
                                   nullptr, 0);
    if (wlen <= 0) return std::wstring(in, in_len);
    std::wstring out(wlen, L'\0');
    MultiByteToWideChar(kSpoofedCodePage, 0,
                        sjis.data(), (int)sjis.size(),
                        out.data(), wlen);
    return out;
}

// Skip a sz_or_ord field at *cursor: either 0xFFFF + ordinal, or wide
// string null-terminated. Advance *cursor past it. If translated_out is
// non-null and the field is a string, write the translated wide string
// (NUL-terminated) into it; otherwise write the raw bytes.
void DlgTemplate_SkipOrTranslateString(const uint8_t** cursor,
                                       std::vector<uint8_t>* out) {
    const uint16_t* w = (const uint16_t*)*cursor;
    if (w[0] == 0x0000) {
        // Empty string (null terminator only). Advance past one WORD.
        if (out) {
            out->push_back(0); out->push_back(0);
        }
        *cursor += 2;
        return;
    }
    if (w[0] == 0xFFFF) {
        // Ordinal: 0xFFFF followed by one WORD. Advance 4 bytes total.
        if (out) {
            const uint8_t* src = *cursor;
            out->insert(out->end(), src, src + 4);
        }
        *cursor += 4;
        return;
    }
    // String: walk to NUL.
    size_t len = 0;
    while (w[len] != 0) ++len;
    if (out) {
        std::wstring translated = DialogStringTranslate((const wchar_t*)w, len);
        const uint8_t* tb = (const uint8_t*)translated.c_str();
        out->insert(out->end(), tb, tb + (translated.size() + 1) * sizeof(wchar_t));
    }
    *cursor += (len + 1) * 2;  // string + NUL, 2 bytes per WCHAR
}

// Align scratch buffer to DWORD boundary (DLGITEMTEMPLATE entries are
// DWORD-aligned in the template).
void DlgTemplate_AlignToDword(std::vector<uint8_t>& out) {
    while (out.size() % 4) out.push_back(0);
}

// Translate a DLGTEMPLATE / DLGTEMPLATEEX into a new buffer with all
// embedded strings re-encoded via CP932. Returns empty on parse failure.
std::vector<uint8_t> TranslateDialogTemplate(const uint8_t* raw, size_t size) {
    if (size < sizeof(DLGTEMPLATE)) return {};
    std::vector<uint8_t> out;
    out.reserve(size + 256);

    const uint16_t* head = (const uint16_t*)raw;
    const bool is_ex = (head[0] == 0x0001 && head[1] == 0xFFFF);
    const uint8_t* cursor = raw;
    uint16_t cdit = 0;
    bool has_font = false;
    DWORD style = 0;

    if (is_ex) {
        // DLGTEMPLATEEX header is 26 bytes: 4 (dlgVer+sig) + 4 (helpID)
        // + 4 (exStyle) + 4 (style) + 2 (cDlgItems) + 2 (x) + 2 (y) + 2 (cx) + 2 (cy)
        if (size < 26) return {};
        out.insert(out.end(), cursor, cursor + 26);
        style = *(const DWORD*)(cursor + 12);
        cdit  = *(const uint16_t*)(cursor + 16);
        cursor += 26;
    } else {
        // DLGTEMPLATE header is 18 bytes
        if (size < 18) return {};
        out.insert(out.end(), cursor, cursor + 18);
        style = *(const DWORD*)cursor;
        cdit  = *(const uint16_t*)(cursor + 8);
        cursor += 18;
    }
    has_font = (style & DS_SETFONT) != 0;

    // menu, windowClass, title
    DlgTemplate_SkipOrTranslateString(&cursor, &out);  // menu
    DlgTemplate_SkipOrTranslateString(&cursor, &out);  // class
    DlgTemplate_SkipOrTranslateString(&cursor, &out);  // title

    if (has_font) {
        if (is_ex) {
            // pointsize(2) + weight(2) + italic(1) + charset(1) + typeface
            out.insert(out.end(), cursor, cursor + 6);
            cursor += 6;
        } else {
            // pointsize(2) + typeface
            out.insert(out.end(), cursor, cursor + 2);
            cursor += 2;
        }
        DlgTemplate_SkipOrTranslateString(&cursor, &out);  // typeface
    }

    // DLGITEMTEMPLATE × cdit, each DWORD-aligned.
    for (uint16_t i = 0; i < cdit; ++i) {
        DlgTemplate_AlignToDword(out);
        // Align cursor in source too -- same rule.
        size_t cur_off = (size_t)(cursor - raw);
        while (cur_off % 4) { ++cursor; ++cur_off; }

        if (is_ex) {
            // DLGITEMTEMPLATEEX header: 22 bytes (helpID 4 + exStyle 4 +
            // style 4 + x 2 + y 2 + cx 2 + cy 2 + id 4)
            if (cur_off + 22 > size) return {};
            out.insert(out.end(), cursor, cursor + 22);
            cursor += 22;
        } else {
            // DLGITEMTEMPLATE header: 18 bytes (style 4 + exStyle 4 +
            // x 2 + y 2 + cx 2 + cy 2 + id 2)
            if (cur_off + 18 > size) return {};
            out.insert(out.end(), cursor, cursor + 18);
            cursor += 18;
        }
        DlgTemplate_SkipOrTranslateString(&cursor, &out);  // class
        DlgTemplate_SkipOrTranslateString(&cursor, &out);  // title

        // creation data: WORD count, then (count-2) bytes of payload (count
        // includes the WORD itself per MSDN; we copy as-is).
        size_t off = (size_t)(cursor - raw);
        if (off + 2 > size) return {};
        uint16_t cd = *(const uint16_t*)cursor;
        out.insert(out.end(), cursor, cursor + 2);
        cursor += 2;
        if (cd > 0) {
            // cd is in WORDs, total payload bytes = cd*2 - 2 (cd includes
            // the count WORD itself per some docs; verify per MSDN).
            size_t pay = (size_t)(cd) * 2 - 2;
            if ((size_t)(cursor - raw) + pay > size) return {};
            out.insert(out.end(), cursor, cursor + pay);
            cursor += pay;
        }
    }

    return out;
}

} // namespace

INT_PTR WINAPI Hook_DialogBoxParamA(HINSTANCE inst, LPCSTR template_name,
                                    HWND parent, DLGPROC dlg_proc,
                                    LPARAM init_param) {
    HRSRC res = FindResourceA(inst, template_name, MAKEINTRESOURCEA(5)); // RT_DIALOG = 5
    if (!res) {
        return p_DialogBoxParamA(inst, template_name, parent, dlg_proc, init_param);
    }
    HGLOBAL h = LoadResource(inst, res);
    DWORD sz = SizeofResource(inst, res);
    LPVOID raw = h ? LockResource(h) : nullptr;
    if (!raw || sz == 0) {
        return p_DialogBoxParamA(inst, template_name, parent, dlg_proc, init_param);
    }
    std::vector<uint8_t> translated = TranslateDialogTemplate((const uint8_t*)raw, sz);
    if (translated.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "LocaleSpoof: DialogBoxParamA template translate failed (size=%lu) -- passing through",
                    (unsigned long)sz);
        return p_DialogBoxParamA(inst, template_name, parent, dlg_proc, init_param);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "LocaleSpoof: DialogBoxParamA template re-encoded via CP932 (orig=%lu B → new=%zu B)",
                (unsigned long)sz, translated.size());
    return DialogBoxIndirectParamW(inst, (LPCDLGTEMPLATEW)translated.data(),
                                   parent, dlg_proc, init_param);
}

