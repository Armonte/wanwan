# DirectPlay Stubbing and Removal Plan

This document outlines the plan to remove the dependency on `DPLAYX.dll` by stubbing out all DirectPlay functionality. The goal is to allow the game to run in a single-player mode without requiring DirectPlay to be installed or initialized.

## Summary of Findings

DirectPlay is used exclusively for multiplayer session management (hosting, finding, and joining games). All related logic is initiated from the "Network" option in the main menu, which eventually calls `DialogBoxParamA` to display a network session dialog.

The core of the initialization process occurs within the dialog procedure `network_dialog_proc` located at `0x402ee0`. This function is responsible for creating the main DirectPlay object via a call to `DirectPlayCreate`.

By hooking key functions and manipulating global state variables, we can simulate a successful, single-player DirectPlay session and bypass all network-related code.

## What We Know

### Key Functions

| Function Name           | Address    | Description                                                                                             |
| ----------------------- | ---------- | ------------------------------------------------------------------------------------------------------- |
| `network_dialog_proc`   | `0x402ee0` | The main dialog procedure for handling network UI and DirectPlay initialization.                        |
| `DPlay_HostSession`     | `0x402be0` | Called when the "Host" button is clicked. Responsible for creating a session and the host player.       |
| `DPlay_JoinSession`     | `0x402d27` | Called when the "Join" button is clicked.                                                               |
| `DPlay_EnumSessions`    | `0x402e1c` | Enumerates active game sessions on the local network.                                                   |
| `DirectPlayCreate`      | `0x41b550` | A wrapper that calls the imported `DirectPlayCreate` from `DPLAYX.dll`. This is our primary hook target. |
| `DirectPlayEnumerateA`  | `0x41b556` | A wrapper that calls the imported `DirectPlayEnumerateA` from `DPLAYX.dll`.                               |

### Key Global Variables

| Variable Name         | Address    | Description                                                                 |
| --------------------- | ---------- | --------------------------------------------------------------------------- |
| `g_pDirectPlay`       | `0x424760` | A pointer to the created `IDirectPlay4A` interface object.                  |
| `g_dpidLocalPlayer`   | `0x424768` | The DirectPlay Player ID (DPID) for the local client.                       |
| `g_dpidHostPlayer`    | `0x42476C` | The DPID for the session host. In our stub, this will be the same as local. |

### IDirectPlay4A V-Table Usage

The game interacts with the `IDirectPlay4A` COM object through its v-table. Our stub must implement the following functions:

| Method         | V-Table Offset | Called From         | Description                                        |
| -------------- | -------------- | ------------------- | -------------------------------------------------- |
| `Release`      | `0x08`         | `network_dialog_proc` | Releases the DirectPlay object.                    |
| `CreatePlayer` | `0x18` (`24`)  | `DPlay_HostSession` | Creates a player object and returns a new DPID.    |
| `Open`         | `0x60` (`96`)  | `DPlay_HostSession` | Opens a session for hosting or joining.            |

## Implementation Plan

The strategy is to replace the real `DirectPlayCreate` function with a hook that returns our own fake `IDirectPlay4A` object. This object will have a custom v-table pointing to our stub functions, which simulate the behavior of a successful single-player session.

### Step 1: Hook `DirectPlayCreate`

-   **Target:** `DirectPlayCreate` at `0x41b550`.
-   **Action:** Create a hook named `Hook_DirectPlayCreate` in `dllmain.cpp`.
-   **Logic:**
    1.  Log the call for debugging.
    2.  Do **not** call the original function.
    3.  Instantiate our `FakeDirectPlay` object.
    4.  Write the address of our fake object to the `lplpDP` output parameter, which is the pointer that `g_pDirectPlay` (`0x424760`) will receive.
    5.  Return `S_OK` (0) to signal success.

### Step 2: Implement a Fake `IDirectPlay4A` Interface

-   **Action:** Define two new structs in `dllmain.cpp`: `FakeDirectPlay` and `FakeDirectPlayVTable`.
-   **Structure:**
    -   `FakeDirectPlayVTable` will contain function pointers for `Release`, `CreatePlayer`, `Open`, and any other methods we need to stub.
    -   `FakeDirectPlay` will contain a pointer to an instance of `FakeDirectPlayVTable`.
-   **Implementation:** Create a single static instance of `FakeDirectPlayVTable` and point to it from every `FakeDirectPlay` object we create.

### Step 3: Implement Stub Functions

These functions will be the implementations in our fake v-table.

1.  **`Stub_Release(void* this)`:**
    -   Logs the call.
    -   Frees the `FakeDirectPlay` object (`delete this`).
    -   Returns `S_OK`.

2.  **`Stub_Open(void* this, ...)`:**
    -   This is called by `DPlay_HostSession` (`0x402be0`).
    -   Logs the call and the requested session name.
    -   Performs no actual action.
    -   Returns `S_OK` to indicate the session was "opened" successfully.

3.  **`Stub_CreatePlayer(void* this, DPID* lpdpidPlayer, ...)`:**
    -   This is called by `DPlay_HostSession` (`0x402be0`) right after `Open`.
    -   Logs the call.
    -   Writes a fake player ID (e.g., `1`) to the `lpdpidPlayer` output parameter.
    -   This fake ID will be stored by the game in the `g_dpidLocalPlayer` (`0x424768`) and `g_dpidHostPlayer` (`0x42476C`) globals.
    -   Returns `S_OK`.

### Step 4: Bypass the Network Dialog

-   **Goal:** To make the game automatically "host" a game upon entering the network screen, skipping the dialog entirely.
-   **Target:** `network_dialog_proc` at `0x402ee0`.
-   **Action:** Create a hook `Hook_NetworkDialogProc`.
-   **Logic:**
    1.  When the hook is called with the `WM_INITDIALOG` message (`272`), it means the dialog is about to be shown.
    2.  Inside our hook, instead of letting the dialog initialize, we will directly call the functions that simulate a "Host" button press.
    3.  This involves calling a stripped-down version of the logic from `DPlay_HostSession` (`0x402be0`) to set up the session and player state.
    4.  Finally, return a value that tells Windows the dialog was handled and should be closed immediately (e.g., by calling `EndDialog`).

## What We Still Need to Discover

-   **In-Game Network Logic:** After session setup, the game may try to send or receive data using other `IDirectPlay4A` methods like `Send` or `Receive`. Further analysis will be needed to see if these functions are called and whether they need to be stubbed to prevent crashes during gameplay.
-   **Error Handling Paths:** The game's error handling for failed DirectPlay calls needs to be fully mapped to ensure our stubs don't trigger unexpected failure states. 