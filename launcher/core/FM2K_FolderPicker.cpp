// FM2K_FolderPicker.cpp -- see the header. Lifted verbatim from
// launcher_ui_settings.cpp (2026-09-02) so the Slint launcher gets the same
// dialog instead of SDL's SHBrowseForFolder fallback.
#include "FM2K_FolderPicker.h"

#include <shobjidl.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace fm2k::folder_picker {
namespace {

std::mutex               g_mtx;
std::vector<std::string> g_result;
std::atomic<bool>        g_ready{false};
std::atomic<bool>        g_open{false};

}  // namespace

void OpenAsync(HWND hwnd) {
    if (g_open.exchange(true)) return;  // one dialog at a time
    std::thread([hwnd]() {
        std::vector<std::string> picked;
        if (SUCCEEDED(CoInitializeEx(nullptr,
                COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            IFileOpenDialog* dlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                           CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
                                           reinterpret_cast<void**>(&dlg))) && dlg) {
                DWORD opts = 0;
                dlg->GetOptions(&opts);
                dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT |
                                FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                if (SUCCEEDED(dlg->Show(hwnd))) {
                    IShellItemArray* items = nullptr;
                    if (SUCCEEDED(dlg->GetResults(&items)) && items) {
                        DWORD count = 0;
                        items->GetCount(&count);
                        for (DWORD i = 0; i < count; ++i) {
                            IShellItem* item = nullptr;
                            if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                                PWSTR wpath = nullptr;
                                if (SUCCEEDED(item->GetDisplayName(
                                        SIGDN_FILESYSPATH, &wpath)) && wpath) {
                                    int n = WideCharToMultiByte(CP_UTF8, 0, wpath,
                                                -1, nullptr, 0, nullptr, nullptr);
                                    if (n > 1) {
                                        std::string s(static_cast<size_t>(n - 1), '\0');
                                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1,
                                                s.data(), n, nullptr, nullptr);
                                        picked.push_back(std::move(s));
                                    }
                                    CoTaskMemFree(wpath);
                                }
                                item->Release();
                            }
                        }
                        items->Release();
                    }
                }
                dlg->Release();
            }
            CoUninitialize();
        }
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_result = std::move(picked);
        }
        g_ready.store(true, std::memory_order_release);
        g_open.store(false, std::memory_order_release);
    }).detach();
}

bool Busy() { return g_open.load(std::memory_order_acquire); }

bool Take(std::vector<std::string>* out) {
    if (!g_ready.exchange(false, std::memory_order_acq_rel)) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (out) out->swap(g_result);
    g_result.clear();
    return true;
}

}  // namespace fm2k::folder_picker
