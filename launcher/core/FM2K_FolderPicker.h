// FM2K_FolderPicker.h -- the native "pick a folder" dialog, shared by both
// launchers. IFileOpenDialog with FOS_PICKFOLDERS: the modern Explorer-style
// picker with a path bar, search, and multi-select. Not SHBrowseForFolder
// (the stubby "Browse For Folder" tree), which is what SDL3's folder dialog
// turns into on Windows.
//
// It MUST run ASYNC. IFileOpenDialog::Show is modal to the parent window and
// drives a message loop the parent window's owning thread (our render thread)
// has to keep pumping. Blocking the render thread for the result (e.g.
// join()) DEADLOCKS -- the dialog waits on the render thread to service its
// messages, the render thread waits on the dialog to finish. So: open it on a
// detached STA worker, keep rendering, and collect the result with Take() on
// a later frame -- the same shape as SDL's own async dialog API.
#pragma once
#include <string>
#include <vector>

#include <windows.h>

namespace fm2k::folder_picker {

// Open the picker over `parent` (may be null). One dialog at a time: a
// second call while one is up is ignored.
void OpenAsync(HWND parent);

// True from OpenAsync until the user closes the dialog.
bool Busy();

// Collect the folders the user picked (UTF-8). Returns true once per
// dialog, when it has closed; `out` is empty when it was cancelled.
bool Take(std::vector<std::string>* out);

}  // namespace fm2k::folder_picker
