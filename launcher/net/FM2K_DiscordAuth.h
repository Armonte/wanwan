// FM2K_DiscordAuth -- launcher-side Discord OAuth pairing flow.
//
// Pairing model:
//   1. Begin() asks the hub for (pairing_code, authorize_url) via
//      GET http://<hub>/pair/begin. The hub mints a pairing_code
//      and constructs the Discord authorize URL on our behalf -- that
//      keeps the OAuth client_id (and would-be code_challenge) entirely
//      server-side.
//   2. Begin() shells the OS browser to authorize_url (ShellExecute
//      "open"). User clicks Authorize on Discord, which redirects to
//      http://<hub>/discord_oauth_callback?code=...&state=<pairing_code>.
//      Hub validates roles, mints a hub_token.
//   3. Poll() hits GET http://<hub>/pair/<pairing_code> on a worker
//      thread until status=ok (returns hub_token + nick) or expired/
//      error/timeout. UI calls IsReady() / TakeResult() each frame.
//   4. Token persists in %APPDATA%\FM2K_Rollback\discord_auth.json.
//      LoadCachedToken() reads it on startup; SignOut() wipes it.
#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>

namespace fm2k::discord_auth {

struct CachedAuth {
    std::string hub_token;
    std::string discord_user_id;
    // Custom display nick the user set in the launcher's Hub panel. Empty
    // if they've never customized; falls back to discord_global_name.
    std::string nick;
    // Authoritative Discord global_name from the OAuth identify call.
    // Stored separately from `nick` so the user can flip back to "use my
    // Discord name" at any time without losing the custom override.
    std::string discord_global_name;
    // When true, the launcher sends discord_global_name to the hub.
    // When false, sends `nick`. Default true so fresh installs match the
    // user's Discord identity by default.
    bool        use_discord_name = true;
    bool        valid = false;
};

// Read / write %APPDATA%\FM2K_Rollback\discord_auth.json. Returns
// {valid=false} if no cache exists or it's malformed.
CachedAuth   LoadCached();
bool         SaveCached(const CachedAuth& a);
void         ClearCached();

// Pairing flow. hub_base_url is e.g. "https://hub.2dfm.org"
// (no trailing slash). Begin() returns a Pairing handle that the UI
// polls each frame.
struct Pairing {
    enum class Status { Pending, Ok, Expired, Error };

    // Snapshot of current state. Cheap to call from UI every frame.
    Status      status() const;
    std::string error_detail() const;   // human-readable, only when Error
    CachedAuth  result() const;         // valid only when Ok
    // The Discord authorize URL as returned by /pair/begin. Empty until
    // the worker has fetched it; populated as soon as `status()` flips
    // out of "Pending" with a code/url-known state. UI surfaces this so
    // a user whose default-browser auto-launch failed (admin process
    // launching a non-admin browser, no http handler registered, AV
    // blocking spawn, etc.) can still copy/paste the URL manually.
    std::string authorize_url() const;
    std::string pairing_code() const;
    // True iff the worker tried to spawn the browser and BOTH the
    // ShellExecute("open") attempt and the cmd /c start fallback
    // returned <=32 (failure). UI uses this to flip the modal into
    // "the browser did not open -- paste this URL" mode and auto-copy
    // the URL to the clipboard, instead of showing the optimistic
    // "Browser opened. Click Authorize on Discord..." status.
    bool        browser_open_failed() const;
    void        Cancel();
    ~Pairing();

private:
    friend Pairing* Begin(const std::string& hub_base_url);

    std::string                     hub_base_url_;
    std::string                     pairing_code_;
    std::string                     authorize_url_;
    std::atomic<int>                status_{0};
    std::string                     error_;
    CachedAuth                      result_;
    std::atomic<bool>               cancel_{false};
    std::atomic<bool>               browser_open_failed_{false};
    std::thread                     worker_;
    mutable std::mutex              mtx_;
};

// Kicks off the pairing. Returns ownership of the Pairing object;
// UI must keep it alive while polling. Returns nullptr only on
// allocation failure.
Pairing* Begin(const std::string& hub_base_url);

}  // namespace fm2k::discord_auth
