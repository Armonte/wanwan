### Decompiled Code (with renamed variables)

```c
/* line: 0, address: 0x415220 */ BOOL RenderDebugConsole()
/* line: 1 */ {
/* line: 2 */   HDC hDeviceContext; // esi
/* line: 3 */   int debugStringLen; // eax
/* line: 4 */   int debugStringLen_2; // eax
/* line: 5 */   char *pConsoleLineIter; // eax
/* line: 6 */   int yPos; // ebp
/* line: 7 */   int lineBufferLen; // eax
/* line: 8 */   int lineBufferLen_2; // eax
/* line: 9 */   int lineBufferLen_3; // eax
/* line: 10 */   int lineBufferLen_4; // eax
/* line: 11 */   int lineBufferLen_5; // eax
/* line: 12 */   char *pCurrentConsoleLine; // [esp+10h] [ebp-10Ch]
/* line: 13 */   HGDIOBJ hOriginalFont; // [esp+14h] [ebp-108h]
/* line: 14 */   HFONT hFont; // [esp+18h] [ebp-104h]
/* line: 15 */   CHAR szLineBuffer[256]; // [esp+1Ch] [ebp-100h] BYREF
/* line: 16 */ 
/* line: 17, address: 0x41523c */   hDeviceContext = g_hDeviceContext;
/* line: 18, address: 0x41525a */   hFont = CreateFontA(14, 6, 0, 0, 0, 0, 0, 0, 0x80u, 0, 0, 0, 0x30u, g_DebugFontName);
/* line: 19, address: 0x415267 */   hOriginalFont = SelectObject(hDeviceContext, hFont);
/* line: 20, address: 0x41526b */   SetBkMode(hDeviceContext, 1);
/* line: 21, address: 0x415284 */   if ( g_EnableDebugString )
/* line: 22 */   {
/* line: 23, address: 0x415292 */     SetTextColor(hDeviceContext, 0xFF0000u);
/* line: 24, address: 0x415299 */     debugStringLen = lstrlenA(g_DebugString);
/* line: 25, address: 0x4152a9 */     TextOutA(hDeviceContext, 9, 465, g_DebugString, debugStringLen);
/* line: 26, address: 0x4152b1 */     SetTextColor(hDeviceContext, 0xFFFFu);
/* line: 27, address: 0x4152b8 */     debugStringLen_2 = lstrlenA(g_DebugString);
/* line: 28, address: 0x4152c8 */     TextOutA(hDeviceContext, 8, 464, g_DebugString, debugStringLen_2);
/* line: 29 */   }
/* line: 30, address: 0x4152ca */   pConsoleLineIter = g_DebugConsoleLines;
/* line: 31, address: 0x4152cf */   yPos = 448;
/* line: 32, address: 0x4152d4 */   pCurrentConsoleLine = g_DebugConsoleLines;
/* line: 33, address: 0x415386 */   do
/* line: 34 */   {
/* line: 35, address: 0x4152d8 */     if ( *(pConsoleLineIter + 16) ) // Check if the line is active
/* line: 36 */     {
/* line: 37, address: 0x4152ee */       sprintf(szLineBuffer, "%s", pConsoleLineIter);
/* line: 38, address: 0x4152f9 */       SetTextColor(hDeviceContext, 0); // Shadow color
/* line: 39, address: 0x415304 */       lineBufferLen = lstrlenA(szLineBuffer);
/* line: 40, address: 0x415313 */       TextOutA(hDeviceContext, 0, yPos + 1, szLineBuffer, lineBufferLen);
/* line: 41, address: 0x41531a */       lineBufferLen_2 = lstrlenA(szLineBuffer);
/* line: 42, address: 0x415326 */       TextOutA(hDeviceContext, 1, yPos, szLineBuffer, lineBufferLen_2);
/* line: 43, address: 0x41532d */       lineBufferLen_3 = lstrlenA(szLineBuffer);
/* line: 44, address: 0x41533c */       TextOutA(hDeviceContext, 0, yPos - 1, szLineBuffer, lineBufferLen_3);
/* line: 45, address: 0x415343 */       lineBufferLen_4 = lstrlenA(szLineBuffer);
/* line: 46, address: 0x41534f */       TextOutA(hDeviceContext, -1, yPos, szLineBuffer, lineBufferLen_4);
/* line: 47, address: 0x41535a */       SetTextColor(hDeviceContext, *(pCurrentConsoleLine - 1)); // Set text color
/* line: 48, address: 0x415365 */       lineBufferLen_5 = lstrlenA(szLineBuffer);
/* line: 49, address: 0x415371 */       TextOutA(hDeviceContext, 0, yPos, szLineBuffer, lineBufferLen_5);
/* line: 50, address: 0x415373 */       pConsoleLineIter = pCurrentConsoleLine;
/* line: 51, address: 0x415377 */       yPos -= 16;
/* line: 52 */     }
/* line: 53, address: 0x41537a */     pConsoleLineIter += 72;
/* line: 54, address: 0x415382 */     pCurrentConsoleLine = pConsoleLineIter;
/* line: 55 */   }
/* line: 56, address: 0x415386 */   while ( pConsoleLineIter < g_DebugConsoleLines_End );
/* line: 57, address: 0x415392 */   SelectObject(hDeviceContext, hOriginalFont);
/* line: 58, address: 0x4153a3 */   return DeleteObject(hFont);
/* line: 59 */ }
```

### Globals Referenced

*   `g_hDeviceContext`: Handle to the window's device context.
*   `g_DebugFontName`: Name of the font to use for rendering. 

---

## `DebugConsole_AddMessage`

**Address:** `0x415190`

This function is responsible for adding a new message to the debug console. It takes a string and a color as input.

### Summary

When a new message is added, this function shifts all existing messages down by one slot in the `g_DebugConsoleLines` buffer. It then copies the new message text into the first slot. The color and a "lifetime" value (which appears to be a countdown timer to control how long the message is displayed) are stored in separate global variables, `g_DebugConsole_NextColor` and `g_DebugConsole_NextLifetime`. This separation of concerns is likely why the decompiler had trouble interpreting the data structures in `RenderDebugConsole`.

### Decompiled Code (with renamed variables)
```c
/* line: 0, address: 0x415190 */ int __cdecl DebugConsole_AddMessage(const char *pszMessage, int color)
/* line: 1 */ {
/* line: 2 */   char *pLineIter; // esi
/* line: 3 */ 
/* line: 4, address: 0x415197 */   if ( !g_debug_mode )
/* line: 5, address: 0x415199 */     return 0;
/* line: 6, address: 0x41519e */   pLineIter = pEndOfBuffer;
/* line: 7, address: 0x4151c9 */   do
/* line: 8 */   {
/* line: 9, address: 0x4151ad */     sprintf(pLineIter, "%s", pLineIter - 72);
/* line: 10, address: 0x4151b8 */     *(pLineIter - 1) = *(pLineIter - 19);
/* line: 11, address: 0x4151be */     *(pLineIter + 16) = *(pLineIter - 2);
/* line: 12, address: 0x4151c1 */     pLineIter -= 72;
/* line: 13 */   }
/* line: 14, address: 0x4151c9 */   while ( pLineIter >= g_DebugConsoleLines_Start );
/* line: 15, address: 0x4151da */   sprintf(g_DebugConsoleLines, "%s", pszMessage);
/* line: 16, address: 0x4151e6 */   g_DebugConsole_NextColor = color;
/* line: 17, address: 0x4151eb */   g_DebugConsole_NextLifetime = 600;
/* line: 18, address: 0x41519b */   return 0;
/* line: 19 */ }
```

### Globals Referenced

*   `pEndOfBuffer`: A pointer to the end of the debug console buffer.
*   `g_DebugConsoleLines_Start`: A pointer to the start of the debug console line buffer.
*   `g_DebugConsoleLines`: A pointer to the first entry in the buffer, where new messages are written.
*   `g_DebugConsole_NextColor`: Stores the color for the *next* message to be added to the console.
*   `g_DebugConsole_NextLifetime`: Stores the lifetime for the *next* message. This seems to be initialized to 600 (frames?).

## `render_game`

**Address:** `0x404dd0`

The `main_window_proc` function contains handlers for other keys when `g_debug_mode` is active:

*   **'p'**: Toggles `g_hit_judge_config`.
*   **'t'**: Sets `g_p2_hp` to 0 or `g_some_hp_related_value` to 100 depending on `g_character_select_mode_flag`.
*   **'u'**: Sets `g_p1_hp` to 0.
*   **'{'**: Increments `g_round_end_flag`.

These should be investigated to fully understand the available debug functionalities. 

---

## `LoadGameDataFile`

**Address:** `0x403d60`

This function is responsible for loading the main game data file. The logic for finding the file changes significantly if debug mode is active.

### Analysis of "GameSystem Open error"

The user reported receiving a "GameSystem Open error" when running the game with any debug flag. The analysis of this function and its caller, `Game_Initialize`, reveals the cause.

In normal mode, the game takes its executable name (e.g., `game.exe`), and attempts to load a corresponding `.kgt` file (e.g., `game.kgt`).

However, if `g_debug_mode` is enabled (or if the executable is named `kgt.exe`), the game completely changes its behavior. It ignores the `.kgt` logic and instead tries to load a different, specific data file whose path is stored at the global address `0x43012C`.

**The error occurs because this special debug data file does not exist in the expected location, causing `CreateFileA` to fail.** The debug flag is working as intended; the problem is a missing file that is required for the debug mode's data loading path.

### Decompiled Code (with renamed variables)

```c
/* line: 0, address: 0x403d60 */ int __cdecl LoadGameDataFile(LPCSTR lpFileName)
/* line: 1 */ {
/* line: 2 */   LPCSTR pszFileName; // edi
/* line: 3 */   HANDLE hFile; // esi
/* line: 4 */   DWORD NumberOfBytesRead; // [esp+Ch] [ebp-408h] BYREF
/* line: 5 */   int bIsTempFileLoaded; // [esp+10h] [ebp-404h]
/* line: 6 */   char szErrorBuffer[256]; // [esp+14h] [ebp-400h] BYREF
/* line: 7 */   CHAR szTempFileName[256]; // [esp+114h] [ebp-300h] BYREF
/* line: 8 */   CHAR szWindowTitle[512]; // [esp+214h] [ebp-200h] BYREF
/* line: 9 */ 
/* line: 10, address: 0x403d69 */   NumberOfBytesRead = 0;
/* line: 11, address: 0x403d71 */   bIsTempFileLoaded = 0;
/* line: 12, address: 0x403d79 */   InitializeGameData();
/* line: 13, address: 0x403d83 */   dword_435460 = 0;
/* line: 14, address: 0x403d8f */   if ( g_debug_mode )
/* line: 15 */   {
/* line: 16, address: 0x403d95 */     pszFileName = lpFileName;
/* line: 17, address: 0x403daa */     sprintf(szTempFileName, "%s.t", lpFileName);
/* line: 18, address: 0x403dd4 */     hFile = CreateFileA(szTempFileName, 0x80000000, 0, 0, 3u, 0x80u, 0);
/* line: 19, address: 0x403dd9 */     if ( hFile == -1 )
/* line: 20 */     {
/* line: 21, address: 0x403df0 */       hFile = CreateFileA(lpFileName, 0x80000000, 0, 0, 3u, 0x80u, 0);
/* line: 22, address: 0x403df5 */       if ( hFile == -1 )
/* line: 23 */       {
/* line: 24, address: 0x403dfd */         sprintf(szErrorBuffer, "GameSystem Open error[%s]", lpFileName);
/* line: 25 */ LABEL_10:
/* line: 26, address: 0x403e68 */         ShowFatalError(szErrorBuffer);
/* line: 27, address: 0x403e84 */         return -1;
/* line: 28 */       }
/* line: 29 */     }
/* line: 30 */     else
/* line: 31 */     {
/* line: 32, address: 0x403dff */       bIsTempFileLoaded = 1;
/* line: 33 */     }
/* line: 34 */   }
/* line: 35 */   else
/* line: 36 */   {
/* line: 37, address: 0x403e85 */     pszFileName = lpFileName;
/* line: 38, address: 0x403ea5 */     hFile = CreateFileA(lpFileName, 0x80000000, 0, 0, 3u, 0x80u, 0);
/* line: 39, address: 0x403eaa */     if ( hFile == -1 )
/* line: 40 */     {
/* line: 41, address: 0x403eb7 */       WinExec(CmdLine, 5u);
/* line: 42, address: 0x403ebf */       PostQuitMessage(0);
/* line: 43, address: 0x403ed0 */       return -1;
/* line: 44 */     }
/* line: 45 */   }
/* line: 46, address: 0x403e0e */   SetFilePointer(hFile, 0, 0, 0);
/* line: 47, address: 0x403e52 */   if ( !ReadFile(hFile, g_GameDataHeader, 0x10u, &NumberOfBytesRead, 0)
/* line: 48 */     || character_data_loader(g_GameDataHeader, hFile)
/* line: 49 */     || !ReadFile(hFile, g_player_file_list, 0x1023Cu, &NumberOfBytesRead, 0) )
/* line: 50 */   {
/* line: 51, address: 0x403e63 */     sprintf(szErrorBuffer, "GameSystem Read error[%s]", pszFileName);
/* line: 52, address: 0x403e63 */     goto LABEL_10;
/* line: 53 */   }
/* line: 54, address: 0x403ed2 */   CloseHandle(hFile);
/* line: 55, address: 0x403edd */   g_bGameDataLoaded = 1;
/* line: 56, address: 0x403ee9 */   if ( g_szGameVersionString )
/* line: 57, address: 0x403f14 */     sprintf(szWindowTitle, "%s", &g_szGameVersionString);
/* line: 58 */   else
/* line: 59, address: 0x403ef8 */     sprintf(szWindowTitle, g_szDefaultVersionString);
/* line: 60, address: 0x403f23 */   if ( g_debug_mode )
/* line: 61, address: 0x403f3a */     sprintf(szWindowTitle, g_szDebugModeFormatString, szWindowTitle);
/* line: 62, address: 0x403f50 */   SetWindowTextA(g_hwnd_parent, szWindowTitle);
/* line: 63, address: 0x403f61 */   sprintf(szErrorBuffer, g_szDataLoadedMessageFormat, pszFileName);
/* line: 64, address: 0x403f70 */   DebugConsole_AddMessage(szErrorBuffer, 14680063);
/* line: 65, address: 0x403f7e */   if ( bIsTempFileLoaded )
/* line: 66, address: 0x403f88 */     DeleteFileA(szTempFileName);
/* line: 67, address: 0x403e7b */   return 0;
/* line: 68 */ }
``` 

---

## `Game_Initialize`

**Address:** `0x409A60`

This is the main initialization function for the game. It is responsible for parsing the command line, determining which data file to load, loading it, and then setting up the initial game state based on the loaded data and any debug flags.

### Summary of Functionality

1.  **Game Data Loading:**
    *   It parses the command line to get the executable's name.
    *   If the executable is named `kgt.exe` or if any debug flag is active, it attempts to load a special data file specified by the global `g_szDebugDataFileName`. This is the source of the "GameSystem Open error" when the file is missing.
    *   Otherwise, it appends `.kgt` to the executable's name and loads that file.
2.  **State Initialization:**
    *   It initializes various game systems by calling `bitmap_loader` and setting initial values for many global state variables.
3.  **Debug Mode Setup:**
    *   If debug mode is active, it checks the specific mode (`g_debug_mode` == 1 or 3) and sets up different game scenarios by creating specific game objects and setting player data. This is how debug modes like "Test" or "Fast" are configured.

### Decompiled Code (with renamed variables)

```c
/* line: 0, address: 0x409a60 */ void Game_Initialize()
/* line: 1 */ {
/* line: 2 */   int idx; // esi
/* line: 3 */   const char *CommandLineA; // eax
/* line: 4 */   char *pCmdLineChar; // ecx
/* line: 5 */   CHAR c; // al
/* line: 6 */   bool v4; // zf
/* line: 7 */   int bHasBackslash; // edx
/* line: 8 */   int idx2; // eax
/* line: 9 */   char c2; // cl
/* line: 10 */   int idx3; // edx
/* line: 11 */   int idx4; // ecx
/* line: 12 */   CHAR c3; // al
/* line: 13 */   int cfg_val3; // ebp
/* line: 14 */   int cfg_val1; // esi
/* line: 15 */   int pPlayerState; // edx
/* line: 16 */   CHAR szPathBuffer; // [esp+10h] [ebp-200h] BYREF
/* line: 17 */   char v15; // [esp+11h] [ebp-1FFh]
/* line: 18 */   char v16; // [esp+12h] [ebp-1FEh]
/* line: 19 */   char v17[2]; // [esp+10Eh] [ebp-102h]
/* line: 20 */   char szCmdLineBuffer[256]; // [esp+110h] [ebp-100h] BYREF
/* line: 21 */ 
/* line: 22, address: 0x409a6b */   if ( !*(g_current_game_object_ptr + 338) )
/* line: 23 */   {
/* line: 24, address: 0x409a85 */     *(g_current_game_object_ptr + 338) = 1;
/* line: 25, address: 0x409aa2 */     memset(g_player_character_selection, 0xFFu, 0x20u);
/* line: 26, address: 0x409aa9 */     g_Unknown_424708 = 0;
/* line: 27, address: 0x409aaf */     g_Unknown_447F28 = 1;
/* line: 28, address: 0x409ab5 */     bitmap_loader(sprite_data, Name, FileName);
/* line: 29, address: 0x409ac4 */     if ( g_debug_mode )
/* line: 30, address: 0x409ac4 */       goto LABEL_16;
/* line: 31, address: 0x409aca */     idx = 0;
/* line: 32, address: 0x409acc */     CommandLineA = GetCommandLineA();
/* line: 33, address: 0x409ae0 */     sprintf(szCmdLineBuffer, "%s", CommandLineA);
/* line: 34, address: 0x409ae8 */     for ( pCmdLineChar = szCmdLineBuffer; ; ++pCmdLineChar )
/* line: 35 */     {
/* line: 36, address: 0x409aef */       c = *pCmdLineChar;
/* line: 37, address: 0x409af1 */       v4 = *pCmdLineChar == 46;
/* line: 38, address: 0x409af3 */       *(&szPathBuffer + idx) = *pCmdLineChar;
/* line: 39, address: 0x409af7 */       if ( v4 )
/* line: 40, address: 0x409af7 */         break;
/* line: 41, address: 0x409afb */       if ( c == 92 )
/* line: 42, address: 0x409afd */         idx = -1;
/* line: 43, address: 0x409b00 */       ++idx;
/* line: 44 */     }
/* line: 45, address: 0x409b2a */     if ( ((*(&szPathBuffer + idx) = 0, szPathBuffer == 75) || szPathBuffer == 107)
/* line: 46 */       && (v15 == 71 || v15 == 103)
/* line: 47 */       && (v16 == 84 || v16 == 116) )
/* line: 48 */     {
/* line: 49 */ LABEL_16:
/* line: 50, address: 0x409b67 */       bHasBackslash = 0;
/* line: 51, address: 0x409b69 */       idx2 = 0;
/* line: 52, address: 0x409b93 */       do
/* line: 53 */       {
/* line: 54, address: 0x409b6b */         c2 = g_exe_path[idx2];
/* line: 55, address: 0x409b73 */         v17[idx2] = c2;
/* line: 56, address: 0x409b7f */         if ( !bHasBackslash && c2 == 92 )
/* line: 57 */         {
/* line: 58, address: 0x409b81 */           bHasBackslash = 1;
/* line: 59, address: 0x409b83 */           v17[idx2 + 1] = 0;
/* line: 60 */         }
/* line: 61, address: 0x409b8a */         --idx2;
/* line: 62 */       }
/* line: 63, address: 0x409b93 */       while ( idx2 + 254 >= 0 );
/* line: 64, address: 0x409b9a */       SetCurrentDirectoryA(&szPathBuffer);
/* line: 65, address: 0x409ba0 */       idx3 = 0;
/* line: 66, address: 0x409ba2 */       for ( idx4 = 0; idx4 < 256; ++idx4 )
/* line: 67 */       {
/* line: 68, address: 0x409ba4 */         c3 = g_szDebugDataFileName[idx4];
/* line: 69, address: 0x409bac */         *(&szPathBuffer + idx3) = c3;
/* line: 70, address: 0x409bb0 */         if ( !c3 )
/* line: 71, address: 0x409bb0 */           break;
/* line: 72, address: 0x409bb4 */         if ( c3 == 92 )
/* line: 73, address: 0x409bb6 */           idx3 = -1;
/* line: 74, address: 0x409bba */         ++idx3;
/* line: 75 */       }
/* line: 76, address: 0x409bc8 */       if ( LoadGameDataFile(&szPathBuffer) )
/* line: 77, address: 0x409bd2 */         goto LABEL_15;
/* line: 78 */     }
/* line: 79 */     else
/* line: 80 */     {
/* line: 81, address: 0x409b3b */       sprintf(&szPathBuffer, "%s.kgt", &szPathBuffer);
/* line: 82, address: 0x409b45 */       if ( LoadGameDataFile(&szPathBuffer) )
/* line: 83 */       {
/* line: 84 */ LABEL_15:
/* line: 85, address: 0x409b55 */         PostQuitMessage(0);
/* line: 86, address: 0x409b66 */         return;
/* line: 87 */       }
/* line: 88 */     }
/* line: 89, address: 0x409be6 */     cfg_val3 = g_iConfigP1_Color;
/* line: 90, address: 0x409bec */     cfg_val1 = g_iConfigP1_Character;
/* line: 91, address: 0x409bfe */     g_player_character_selection[0] = g_iConfigP1_Character;
/* line: 92, address: 0x409c04 */     memset32(&g_iPlayer1_Color_Selection, g_iConfigP1_Color, 7u);
/* line: 93, address: 0x409c13 */     g_fm2k_game_mode = nGameMode;
/* line: 94, address: 0x409c19 */     if ( g_debug_mode )
/* line: 95 */     {
/* line: 96, address: 0x409c20 */       if ( g_debug_mode == 1 )
/* line: 97 */       {
/* line: 98, address: 0x409cd3 */         create_game_object(12, 127, 0, 0);
/* line: 99 */       }
/* line: 100, address: 0x409c29 */       else if ( g_debug_mode == 3 )
/* line: 101 */       {
/* line: 102, address: 0x409c3c */         pPlayerState = &g_player_state_flags;
/* line: 103, address: 0x409c41 */         memset(g_player_character_selection, 0xFFu, 0x20u);
/* line: 104, address: 0x409c55 */         do
/* line: 105 */         {
/* line: 106, address: 0x409c43 */           *pPlayerState = -1;
/* line: 107, address: 0x409c49 */           pPlayerState += 57407;
/* line: 108 */         }
/* line: 109, address: 0x409c55 */         while ( pPlayerState < 5570435 );
/* line: 110, address: 0x409c63 */         g_character_select_mode_flag = 1;
/* line: 111, address: 0x409c69 */         g_player_character_selection[0] = cfg_val1;
/* line: 112, address: 0x409c6f */         g_iPlayer1_Color_Selection = cfg_val3;
/* line: 113, address: 0x409c75 */         g_bPlayer1_HasStatus = 0;
/* line: 114, address: 0x409c7b */         g_bPlayer2_HasStatus = 0;
/* line: 115, address: 0x409c86 */         if ( g_iConfigP2_Character )
/* line: 116 */         {
/* line: 117, address: 0x409c88 */           g_bPlayer1_HasStatus = 1;
/* line: 118, address: 0x409c8e */           g_iPlayer1_StatusValue = 100;
/* line: 119, address: 0x409c94 */           g_iPlayer1_Character = g_iConfigP2_Character;
/* line: 120 */         }
/* line: 121, address: 0x409ca0 */         if ( g_iConfigP2_Color )
/* line: 122 */         {
/* line: 123, address: 0x409ca2 */           g_bPlayer2_HasStatus = 1;
/* line: 124, address: 0x409ca8 */           g_iPlayer2_StatusValue = 100;
/* line: 125, address: 0x409cae */           g_iPlayer2_Character = g_iConfigP2_Color;
/* line: 126 */         }
/* line: 127, address: 0x409cb7 */         g_player_state_flags = 0;
/* line: 128, address: 0x409cbd */         g_bPlayer2_IsReady = 1;
/* line: 129, address: 0x409cc3 */         g_game_paused = 0;
/* line: 130, address: 0x409ccb */         create_game_object(14, 127, 0, 0);
/* line: 131 */       }
/* line: 132 */     }
/* line: 133 */     else
/* line: 134 */     {
/* line: 135, address: 0x409cdb */       create_game_object(17, 127, 0, 0);
/* line: 136 */     }
/* line: 137, address: 0x409ce9 */     *g_current_game_object_ptr = 1;
/* line: 138 */   }
/* line: 139 */ }
``` 