// app.rs -- the app-data half of the C ABI: everything the launcher
// pushes into the UI (rosters, chat, replays, match state, settings)
// and everything it drains back out (button presses, typed text).
//
// Split from lib.rs at the 1000-line ceiling. lib.rs keeps the window,
// the software surface and the input devices; nothing here knows those
// exist beyond reaching the shared STATE.
// The Slint-generated types (Peer, ChatLine, ReplayRow, LibRow,
// MatchStage, AppWindow) are emitted at the crate root by
// include_modules!, so they are reached through the crate rather than
// redeclared here.
use crate::{BrowseRow, CaptureShot, ChatLine, InputPlayer, InputRow, LibRow, MatchStage, Peer, RailGame, ReplayRow, SettingRow, STATE};
use std::cell::RefCell;

thread_local! {
    // The folder dialog is a HOST concern, not a Slint one: SDL owns the
    // window and therefore owns the native dialog. So the Slint callback
    // only raises a flag, and the host drains it on its next loop turn.
    //
    // Deliberately a flag rather than a Rust->C function pointer. A
    // callback would run inside Slint's event dispatch, which is exactly
    // where a modal native dialog must NOT be opened: it would pump a
    // nested message loop while Slint is mid-update.
    pub(crate) static BROWSE_PENDING: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    pub(crate) static ONBOARD_DONE: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    // Roster index the user clicked to challenge, +1 so that 0 can mean
    // "nothing pending". -1 would work too, but every other take_* here
    // returns 0-for-nothing and diverging on one of them is how a caller
    // ends up challenging peer 0 on every idle frame.
    pub(crate) static CHALLENGE_PENDING: std::cell::Cell<i32> = const { std::cell::Cell::new(0) };
    // Which button the user pressed in the match modal, 0 for none. One
    // slot rather than eight flags: the stages are mutually exclusive, so
    // two of these can never be pending at once, and a single slot makes
    // that impossible to get wrong rather than merely unlikely.
    pub(crate) static MATCH_ACTION: std::cell::Cell<i32> = const { std::cell::Cell::new(0) };
    // Roster action, encoded as (index << 4) | verb so one slot carries
    // both. 0 = nothing pending. Verbs: 1 spectate, 2 ignore. Challenge
    // keeps its own slot because it predates this and the host already
    // drains it.
    pub(crate) static ROSTER_ACTION: std::cell::Cell<i32> = const { std::cell::Cell::new(0) };
    pub(crate) static LAUNCH_PENDING: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    // A rail chip's "leave room": library index + 1, 0 when none.
    pub(crate) static LEAVE_OUT: std::cell::Cell<i32> = const { std::cell::Cell::new(0) };
    // Text the user sent in the lobby channel, waiting to go on the wire.
    pub(crate) static CHAT_OUT: RefCell<Option<String>> = const { RefCell::new(None) };
    pub(crate) static FILTER_OUT: RefCell<Option<String>> = const { RefCell::new(None) };
    pub(crate) static CONFIG_DIRTY: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    // (key, value) edits from the Config view, oldest first. The host drains
    // them, writes the store, and republishes the rows.
    pub(crate) static SETTINGS_OUT: RefCell<std::collections::VecDeque<(String, String)>> =
        const { RefCell::new(std::collections::VecDeque::new()) };
    // Library art: the thumb path per row, the current tile size, and a
    // generation so a stale resize result cannot land over a newer one.
    pub(crate) static LIB_THUMBS: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    // Per row, the approved slideshow shots (thumb paths), from field 9.
    pub(crate) static LIB_EXTRAS: RefCell<Vec<Vec<String>>> = const { RefCell::new(Vec::new()) };
    pub(crate) static ART_GEN: std::cell::Cell<u64> = const { std::cell::Cell::new(1) };
    pub(crate) static GALLERY_GEN: std::cell::Cell<u64> = const { std::cell::Cell::new(1) };
    // The tile's inner size (what the art is prefiltered to), and a pending
    // size with the moment it was set: a window being dragged passes
    // through dozens of sizes, and only the one it settles on is worth
    // 120 Lanczos passes.
    pub(crate) static TILE_SIZE: std::cell::Cell<(u32, u32)> = const { std::cell::Cell::new((318, 238)) };
    pub(crate) static TILE_PENDING: std::cell::Cell<Option<((u32, u32), std::time::Instant)>> =
        const { std::cell::Cell::new(None) };
    // The last art delivered per thumb path, so a rebuilt library row
    // starts with a picture (a resize or a filter must never flash NO
    // PREVIEW).
    pub(crate) static ART_LAST: RefCell<std::collections::HashMap<String, (slint::Image, slint::Image, [slint::Image; 3])>> =
        RefCell::new(std::collections::HashMap::new());
    // Binder clicks ("bind:0:4:0", "cancel", ...), oldest first.
    pub(crate) static INPUT_OUT: RefCell<std::collections::VecDeque<String>> =
        const { RefCell::new(std::collections::VecDeque::new()) };
    // (path, match to seek to) for a replay the user asked to watch.
    pub(crate) static WATCH_OUT: RefCell<Option<(String, i32)>> = const { RefCell::new(None) };
    pub(crate) static CONNECT_HUB: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
    pub(crate) static WINDOW_ACTION: std::cell::Cell<i32> = const { std::cell::Cell::new(0) };
    // -1 when nothing pending; otherwise the source the user picked.
    pub(crate) static REPLAY_SOURCE: std::cell::Cell<i32> = const { std::cell::Cell::new(-1) };
}

/// 1 if the UI asked for a folder picker since the last call, else 0.
/// Consumes the request.
#[no_mangle]
pub extern "C" fn slint_ui_take_browse_request() -> i32 {
    BROWSE_PENDING.with(|f| i32::from(f.replace(false)))
}

/// 1 if onboarding completed since the last call. Consumes the signal.
#[no_mangle]
pub extern "C" fn slint_ui_take_onboarding_done() -> i32 {
    ONBOARD_DONE.with(|f| i32::from(f.replace(false)))
}

/// Roster index the user asked to challenge since the last call, or -1 for
/// none. Consumes the request.
#[no_mangle]
pub extern "C" fn slint_ui_take_challenge() -> i32 {
    CHALLENGE_PENDING.with(|f| f.replace(0)) - 1
}

/// Enter or leave the in-game session view, and set what it shows. The
/// stats are live telemetry rather than a result, so this is separate from
/// both set_match (the modal stages) and set_match_result (the summary
/// after) -- the three describe different moments and sharing an entry
/// point would let one moment's numbers show up in another.
#[no_mangle]
pub extern "C" fn slint_ui_set_ingame(
    on: i32,
    round: i32,
    rounds_total: i32,
    elapsed_s: i32,
    rtt: i32,
    rollbacks: i32,
    drops: i32,
) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_ingame(on != 0);
            st.ui.set_match_round(round);
            st.ui.set_match_rounds_total(rounds_total);
            st.ui.set_match_elapsed(elapsed_s);
            st.ui.set_match_rtt(rtt);
            st.ui.set_match_rollbacks(rollbacks);
            st.ui.set_match_drops(drops);
        }
    });
}

/// Drain a "watch this replay" request. Writes the file path into `buf`
/// and returns the match to seek to (0 = play from the start), or -1 when
/// nothing is pending.
///
/// # Safety
/// `buf` must be writable for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_take_watch(
    buf: *mut std::os::raw::c_char,
    cap: i32,
) -> i32 {
    if buf.is_null() || cap <= 0 {
        return -1;
    }
    let Some((path, seek)) = WATCH_OUT.with(|w| w.borrow_mut().take()) else {
        return -1;
    };
    let bytes = path.as_bytes();
    let mut n = bytes.len().min(cap as usize - 1);
    while n > 0 && !path.is_char_boundary(n) {
        n -= 1;
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
    seek
}

/// Select a Config tab, and the replay source, from outside the UI.
/// Every screen has to be reachable without a pointer: the capture
/// harness photographs all of them, and synthetic clicks were never
/// reliable enough to drive that.
#[no_mangle]
pub extern "C" fn slint_ui_set_config_tab(tab: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_config_tab(tab);
        }
    });
}

#[no_mangle]
pub extern "C" fn slint_ui_set_replay_source(src: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_replay_source(src);
        }
    });
    // Route it through the same drain the tab click uses, so the host
    // does the identical work either way.
    REPLAY_SOURCE.with(|f| f.set(src));
}

/// Which replay source the user picked since the last call, or -1.
#[no_mangle]
pub extern "C" fn slint_ui_take_replay_source() -> i32 {
    REPLAY_SOURCE.with(|f| f.replace(-1))
}

/// A short line beside the replay source tabs -- what is being shown, or
/// why nothing is.
///
/// # Safety
/// `utf8` must be NUL-terminated UTF-8 or null.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_replay_note(utf8: *const std::os::raw::c_char) {
    let v: slint::SharedString = if utf8.is_null() {
        Default::default()
    } else {
        std::ffi::CStr::from_ptr(utf8).to_str().unwrap_or_default().into()
    };
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_replay_note(v);
        }
    });
}

/// Which window button was pressed since the last call: 1 minimise,
/// 2 maximise or restore, 3 close, 0 none. Consumes the press.
#[no_mangle]
pub extern "C" fn slint_ui_take_window_action() -> i32 {
    WINDOW_ACTION.with(|f| f.replace(0))
}

/// Whether the window is maximised, so the restore button can say so.
#[no_mangle]
pub extern "C" fn slint_ui_set_maximized(on: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_is_maximized(on != 0);
        }
    });
}

/// 1 if the user asked to connect or disconnect from the hub since the
/// last call, with the host they typed written into `buf`. The caller
/// decides which of the two it is from its own connection state -- the
/// button is one control with two jobs and the UI does not track that.
///
/// # Safety
/// `buf` must be writable for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_take_connect_hub(
    buf: *mut std::os::raw::c_char,
    cap: i32,
) -> i32 {
    if buf.is_null() || cap <= 0 || !CONNECT_HUB.with(|f| f.replace(false)) {
        return 0;
    }
    let host = STATE.with(|s| {
        let b = s.borrow();
        b.as_ref()
            .map(|st| st.ui.get_hub_host().to_string())
            .unwrap_or_default()
    });
    let bytes = host.as_bytes();
    let mut n = bytes.len().min(cap as usize - 1);
    while n > 0 && !host.is_char_boundary(n) {
        n -= 1;
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
    1
}

/// Seed the hub host box, and set the one-line note beside the status.
///
/// # Safety
/// Both pointers must be NUL-terminated UTF-8 or null.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_hub_host(utf8: *const std::os::raw::c_char) {
    let v: slint::SharedString = if utf8.is_null() {
        Default::default()
    } else {
        std::ffi::CStr::from_ptr(utf8).to_str().unwrap_or_default().into()
    };
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_hub_host(v);
        }
    });
}

/// # Safety
/// `utf8` must be NUL-terminated UTF-8 or null.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_hub_note(utf8: *const std::os::raw::c_char) {
    let v: slint::SharedString = if utf8.is_null() {
        Default::default()
    } else {
        std::ffi::CStr::from_ptr(utf8).to_str().unwrap_or_default().into()
    };
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_hub_note(v);
        }
    });
}

/// The browse body, one line per GRID ROW:
///   "head\textra\tdim\ti0\ti1\ti2\ti3\ti4"
/// A row with a head is a section heading; otherwise it is up to five
/// tiles by library index, -1 for an empty cell.
///
/// Sectioning is the HOST's call because every section is a question about
/// data it holds and the UI does not -- who is online right now, what you
/// played recently, what nobody is playing.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_browse_rows(utf8: *const std::os::raw::c_char) {
    let mut rows: Vec<BrowseRow> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let f: Vec<&str> = line.split('\t').collect();
                let g = |n: usize| -> i32 {
                    f.get(n).and_then(|v| v.parse().ok()).unwrap_or(-1)
                };
                rows.push(BrowseRow {
                    head: f.first().copied().unwrap_or_default().into(),
                    extra: f.get(1).copied().unwrap_or_default().into(),
                    dim: f.get(2).copied().unwrap_or("0") == "1",
                    i0: g(3), i1: g(4), i2: g(5), i3: g(6), i4: g(7),
                });
            }
        }
    }
    let heads = rows.iter().filter(|r| !r.head.is_empty()).count() as i32;
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_browse_heads(heads);
            st.ui
                .set_library_rows(slint::ModelRc::new(slint::VecModel::from(rows)));
        }
    });
}

/// The left rail's chips, most recently played first: one per line as
/// "index\tline one\tline two". The name is shortened HERE, on the host, because
/// Slint cannot slice a string and a full title wrapped into a 62px chip
/// is several lines of two characters. The host owns the ordering because
/// it is the side that knows what was opened and when, and it survives
/// restarts -- the UI would only ever know about this session.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_rail(utf8: *const std::os::raw::c_char) {
    let mut rows: Vec<RailGame> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let mut f = line.split('\t');
                // index \t name \t name2 \t unread (host_library.h)
                let idx: i32 = f.next().unwrap_or("0").parse().unwrap_or(0);
                let name = f.next().unwrap_or_default();
                let name2 = f.next().unwrap_or_default();
                let unread: i32 = f.next().unwrap_or("0").parse().unwrap_or(0);
                rows.push(RailGame {
                    idx,
                    name: name.into(),
                    name2: name2.into(),
                    unread,
                });
            }
        }
    }
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui
                .set_rail_games(slint::ModelRc::new(slint::VecModel::from(rows)));
        }
    });
}

/// A character face, or an empty image when there is none. A missing face
/// is the normal case for a game whose characters have not been swept yet,
/// so it must not be an error -- the row simply shows no face.
fn load_face(path: &str) -> slint::Image {
    if path.is_empty() {
        return slint::Image::default();
    }
    slint::Image::load_from_path(std::path::Path::new(path)).unwrap_or_default()
}

/// Replace the replay list. One file per line as
/// "kind\twhen\tgame\tp1\tp2\tdetail\tsize\tpath\tset_path\tmatch_index",
/// already ordered by the caller -- sets newest first, matches in play
/// order within each. Re-sorting formatted date STRINGS here would be
/// sorting text that happens to look like a date, and the host is the side
/// that holds the real timestamps.
/// by the caller -- the host walked the directories and knows the real
/// timestamps, whereas re-sorting formatted date STRINGS here would be
/// sorting text that happens to look like a date.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_replays(utf8: *const std::os::raw::c_char) {
    let mut rows: Vec<ReplayRow> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let mut f = line.split('\t');
                rows.push(ReplayRow {
                    kind: f.next().unwrap_or_default().into(),
                    when: f.next().unwrap_or_default().into(),
                    game: f.next().unwrap_or_default().into(),
                    p1: f.next().unwrap_or_default().into(),
                    p2: f.next().unwrap_or_default().into(),
                    detail: f.next().unwrap_or_default().into(),
                    size: f.next().unwrap_or_default().into(),
                    path: f.next().unwrap_or_default().into(),
                    set_path: f.next().unwrap_or_default().into(),
                    match_index: f.next().unwrap_or("0").parse().unwrap_or(0),
                    p1_face: load_face(f.next().unwrap_or_default()),
                    p2_face: load_face(f.next().unwrap_or_default()),
                    winner: f.next().unwrap_or("0").parse().unwrap_or(0),
                });
            }
        }
    }
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui
                .set_replays(slint::ModelRc::new(slint::VecModel::from(rows)));
        }
    });
}

/// Publish the Config view's rows as TSV, one row per line:
/// tab \t key \t kind \t label \t hint \t value \t value_text \t count \t advanced \t enabled \t onboard
/// (see host_settings.h). Replaces the whole model: the host owns the truth.
///
/// # Safety
/// `utf8` must be a valid NUL-terminated string or null.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_settings(utf8: *const std::os::raw::c_char) {
    let mut rows: Vec<SettingRow> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let f: Vec<&str> = line.split('\t').collect();
                let gi = |n: usize| -> i32 { f.get(n).and_then(|v| v.parse().ok()).unwrap_or(0) };
                let gs = |n: usize| -> slint::SharedString { f.get(n).copied().unwrap_or_default().into() };
                rows.push(SettingRow {
                    tab: gi(0),
                    key: gs(1),
                    kind: gi(2),
                    label: gs(3),
                    hint: gs(4),
                    value: gs(5),
                    value_text: gs(6),
                    count: gi(7),
                    advanced: f.get(8).copied().unwrap_or("0") == "1",
                    enabled: f.get(9).copied().unwrap_or("1") == "1",
                    onboard: f.get(10).copied().unwrap_or("0") == "1",
                });
            }
        }
    }
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_settings(slint::ModelRc::new(slint::VecModel::from(rows)));
        }
    });
}

/// The binder's players, TSV (host_binder.h BinderPlayers).
///
/// # Safety
/// `utf8` must be a valid NUL-terminated string or null.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_input_players(utf8: *const std::os::raw::c_char) {
    let mut rows: Vec<InputPlayer> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let f: Vec<&str> = line.split('\t').collect();
                let gi = |n: usize| -> i32 { f.get(n).and_then(|v| v.parse().ok()).unwrap_or(0) };
                let gs = |n: usize| -> slint::SharedString { f.get(n).copied().unwrap_or_default().into() };
                rows.push(InputPlayer {
                    player: gi(0),
                    device_label: gs(1),
                    device_index: gi(2),
                    device_count: gi(3),
                    socd: gi(4),
                    socd_label: gs(5),
                    per_game: f.get(6).copied().unwrap_or("0") == "1",
                    game: gs(7),
                    unmapped: gs(8),
                });
            }
        }
    }
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_input_players(slint::ModelRc::new(slint::VecModel::from(rows)));
        }
    });
}

/// The binder's button rows, TSV (host_binder.h BinderRows).
///
/// # Safety
/// `utf8` must be a valid NUL-terminated string or null.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_input_rows(utf8: *const std::os::raw::c_char) {
    let mut rows: Vec<InputRow> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let f: Vec<&str> = line.split('\t').collect();
                let gi = |n: usize| -> i32 { f.get(n).and_then(|v| v.parse().ok()).unwrap_or(0) };
                let gs = |n: usize| -> slint::SharedString { f.get(n).copied().unwrap_or_default().into() };
                rows.push(InputRow {
                    player: gi(0),
                    bit: gi(1),
                    name: gs(2),
                    primary: gs(3),
                    alt: gs(4),
                    capturing: gi(5),
                    required: f.get(6).copied().unwrap_or("0") == "1",
                });
            }
        }
    }
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_input_rows(slint::ModelRc::new(slint::VecModel::from(rows)));
        }
    });
}

/// Pop the oldest binder action into `buf`. 1 when written, 0 when empty.
///
/// # Safety
/// `buf` must be writable for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_take_input_action(buf: *mut std::os::raw::c_char, cap: i32) -> i32 {
    if buf.is_null() || cap <= 0 {
        return 0;
    }
    let Some(a) = INPUT_OUT.with(|q| q.borrow_mut().pop_front()) else {
        return 0;
    };
    let bytes = a.as_bytes();
    let mut n = bytes.len().min(cap as usize - 1);
    while n > 0 && !a.is_char_boundary(n) {
        n -= 1;
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
    1
}

/// The current view (0 lobby, 1 browse, 2 replays, 3 config), for the host's
/// hit-test: the top bar's controls differ per view.
#[no_mangle]
pub extern "C" fn slint_ui_view() -> i32 {
    STATE.with(|s| {
        let b = s.borrow();
        b.as_ref().map(|st| st.ui.get_view()).unwrap_or(0)
    })
}

const RAIL_CHIP: (u32, u32) = (70, 52);   // 72x54 chip minus its 1px border

fn request_art(thumb_paths: Vec<String>) {
    let gen = ART_GEN.with(|g| { g.set(g.get() + 1); g.get() });
    let tile = TILE_SIZE.with(|t| t.get());
    let extras = LIB_EXTRAS.with(|e| e.borrow().clone());
    let rows: Vec<(usize, String, Vec<String>)> = thumb_paths
        .into_iter()
        .enumerate()
        .map(|(i, p)| (i, p, extras.get(i).cloned().unwrap_or_default()))
        .collect();
    crate::images::request(rows, tile, RAIL_CHIP, gen);
}

/// Browse layout from the host: tiles per row and the exact tile width.
/// A new tile size queues a prefilter pass (images.rs) once the size has
/// held for a quarter second -- see TILE_PENDING.
#[no_mangle]
pub extern "C" fn slint_ui_set_browse_layout(cols: i32, tile_w: i32, tile_h: i32) {
    let inner = ((tile_w - 2).max(1) as u32, (tile_h - 2).max(1) as u32);
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_browse_cols(cols.clamp(1, 5));
            st.ui.set_browse_tile_w(tile_w as f32);
        }
    });
    if TILE_SIZE.with(|t| t.get()) != inner {
        TILE_PENDING.with(|p| p.set(Some((inner, std::time::Instant::now()))));
    } else {
        TILE_PENDING.with(|p| p.set(None));
    }
}

/// The review gallery for the selected game: "name \t thumb \t state" per
/// line (host_captures.h). Replaces the model; the images arrive from the
/// worker through slint_ui_pump_images.
///
/// # Safety
/// `utf8` must be a valid NUL-terminated string or null.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_captures(utf8: *const std::os::raw::c_char) {
    let mut rows: Vec<CaptureShot> = Vec::new();
    let mut paths: Vec<String> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let f: Vec<&str> = line.split('\t').collect();
                rows.push(CaptureShot {
                    name: f.first().copied().unwrap_or_default().into(),
                    state: f.get(2).and_then(|v| v.parse().ok()).unwrap_or(0),
                    image: Default::default(),
                });
                paths.push(f.get(1).copied().unwrap_or_default().to_string());
            }
        }
    }
    let gen = GALLERY_GEN.with(|g| { g.set(g.get() + 1); g.get() });
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_captures(slint::ModelRc::new(slint::VecModel::from(rows)));
        }
    });
    crate::images::request_gallery(paths, GALLERY_CARD, gen);
}

const GALLERY_CARD: (u32, u32) = (198, 148);   // 200x150 card minus its border

/// Apply art the worker finished. Call once per frame on the UI thread.
/// Returns how many rows changed (so the host knows a redraw is due).
#[no_mangle]
pub extern "C" fn slint_ui_pump_images() -> i32 {
    use slint::Model;
    // A settled tile size: prefilter the whole library to it.
    if let Some((size, since)) = TILE_PENDING.with(|p| p.get()) {
        if since.elapsed() >= std::time::Duration::from_millis(250) {
            TILE_PENDING.with(|p| p.set(None));
            TILE_SIZE.with(|t| t.set(size));
            let paths = LIB_THUMBS.with(|t| t.borrow().clone());
            request_art(paths);
        }
    }
    let done = crate::images::take_done();
    if done.is_empty() {
        return 0;
    }
    let gen = ART_GEN.with(|g| g.get());
    let ggen = GALLERY_GEN.with(|g| g.get());
    let mut n = 0;
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return };
        let lib = st.ui.get_library();
        let gallery = st.ui.get_captures();
        for d in done {
            if d.kind == 1 {
                if d.gen != ggen { continue; }
                if let (Some(mut row), Some(t)) = (gallery.row_data(d.row), d.tile.as_ref()) {
                    row.image = crate::images::to_slint(t);
                    gallery.set_row_data(d.row, row);
                    n += 1;
                }
                continue;
            }
            if d.gen != gen {
                continue;   // a size we have since moved past
            }
            let Some(mut row) = lib.row_data(d.row) else { continue };
            // The prefiltered tile is the image; the GPU samples it 1:1.
            if let Some(t) = d.tile.as_ref() { row.thumb = crate::images::to_slint(t); }
            let mut shots = d.extras.iter().map(crate::images::to_slint);
            row.shot1 = shots.next().unwrap_or_default();
            row.shot2 = shots.next().unwrap_or_default();
            row.shot3 = shots.next().unwrap_or_default();
            if let Some(r) = d.rail.as_ref() { row.rail_thumb = crate::images::to_slint(r); }
            let path = LIB_THUMBS.with(|t| t.borrow().get(d.row).cloned().unwrap_or_default());
            if !path.is_empty() {
                ART_LAST.with(|m| { m.borrow_mut().insert(path, (row.thumb.clone(), row.rail_thumb.clone(),
                    [row.shot1.clone(), row.shot2.clone(), row.shot3.clone()])); });
            }
            lib.set_row_data(d.row, row);
            n += 1;
        }
    });
    n
}

/// Whether the DEVELOPER tab is offered.
#[no_mangle]
pub extern "C" fn slint_ui_set_dev_tools(on: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_dev_tools(on != 0);
        }
    });
}

/// Pop the oldest (key, value) edit into the two buffers. Returns 1 when
/// one was written, 0 when the queue is empty.
///
/// # Safety
/// `kbuf` / `vbuf` must be writable for `kcap` / `vcap` bytes.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_take_setting(
    kbuf: *mut std::os::raw::c_char,
    kcap: i32,
    vbuf: *mut std::os::raw::c_char,
    vcap: i32,
) -> i32 {
    if kbuf.is_null() || vbuf.is_null() || kcap <= 0 || vcap <= 0 {
        return 0;
    }
    let Some((k, v)) = SETTINGS_OUT.with(|q| q.borrow_mut().pop_front()) else {
        return 0;
    };
    let put = |src: &str, dst: *mut std::os::raw::c_char, cap: i32| {
        let bytes = src.as_bytes();
        let mut n = bytes.len().min(cap as usize - 1);
        while n > 0 && !src.is_char_boundary(n) {
            n -= 1;
        }
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), dst as *mut u8, n);
        *dst.add(n) = 0;
    };
    put(&k, kbuf, kcap);
    put(&v, vbuf, vcap);
    1
}

/// 1 if any setting was edited since the last call. Consumes the signal,
/// so the host writes the file on change instead of every frame.
#[no_mangle]
pub extern "C" fn slint_ui_take_config_dirty() -> i32 {
    CONFIG_DIRTY.with(|f| i32::from(f.replace(false)))
}

/// Read the display name into `buf` as UTF-8; returns its byte length.
///
/// # Safety
/// `buf` must be writable for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_get_display_name(
    buf: *mut std::os::raw::c_char,
    cap: i32,
) -> i32 {
    if buf.is_null() || cap <= 0 {
        return 0;
    }
    let name = STATE.with(|s| {
        let b = s.borrow();
        b.as_ref()
            .map(|st| st.ui.get_display_name().to_string())
            .unwrap_or_default()
    });
    let bytes = name.as_bytes();
    let mut n = bytes.len().min(cap as usize - 1);
    while n > 0 && !name.is_char_boundary(n) {
        n -= 1;
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
    n as i32
}

/// Set the display name shown in Config and onboarding.
///
/// # Safety
/// `utf8` must be a valid NUL-terminated string or null.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_display_name(utf8: *const std::os::raw::c_char) {
    let name: slint::SharedString = if utf8.is_null() {
        Default::default()
    } else {
        std::ffi::CStr::from_ptr(utf8).to_str().unwrap_or_default().into()
    };
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_display_name(name);
        }
    });
}

/// A roster context-menu action since the last call, or 0 for none.
/// Encoded (index << 4) | verb: 1 spectate, 2 ignore, 3 block.
#[no_mangle]
pub extern "C" fn slint_ui_take_roster_action() -> i32 {
    ROSTER_ACTION.with(|f| f.replace(0))
}

/// Which match-modal button was pressed since the last call, or 0 for none.
/// 1 accept, 2 decline, 3 cancel, 4 rematch, 5 quit, 6 resync, 7 abort,
/// 8 save replay, 9 end match. Consumes the press.
#[no_mangle]
pub extern "C" fn slint_ui_take_match_action() -> i32 {
    MATCH_ACTION.with(|f| f.replace(0))
}

/// 1 if BOOT was pressed since the last call. Consumes the press.
#[no_mangle]
pub extern "C" fn slint_ui_take_launch() -> i32 {
    LAUNCH_PENDING.with(|f| i32::from(f.replace(false)))
}

/// The rail chip the user chose "leave room" on: library index + 1, or 0.
#[no_mangle]
pub extern "C" fn slint_ui_take_leave_room() -> i32 {
    LEAVE_OUT.with(|f| f.replace(0))
}

/// Whether the hub connection is up. Three separate things read this --
/// the compose box, the roster empty state and the status bar -- and it
/// had no setter at all, so all three were stating "offline" as fact.
#[no_mangle]
pub extern "C" fn slint_ui_set_connected(on: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_connected(on != 0);
        }
    });
}

/// Drain the message the user typed in the lobby channel into `buf` as
/// UTF-8. Returns the byte length written, or 0 if nothing is pending.
///
/// The caller owns the send: this only reports what was typed. It does NOT
/// append to the transcript, because the hub broadcasts a message back to
/// its own sender -- echoing locally would show every line twice.
///
/// # Safety
/// `buf` must be writable for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_take_chat_send(
    buf: *mut std::os::raw::c_char,
    cap: i32,
) -> i32 {
    if buf.is_null() || cap <= 0 {
        return 0;
    }
    let Some(text) = CHAT_OUT.with(|c| c.borrow_mut().take()) else {
        return 0;
    };
    let bytes = text.as_bytes();
    // Truncate on a CHARACTER boundary, never mid-sequence: a nick or a
    // message can be CP932-derived, and half a UTF-8 sequence on the wire
    // is a message the hub will reject rather than shorten.
    let mut n = bytes.len().min(cap as usize - 1);
    while n > 0 && !text.is_char_boundary(n) {
        n -= 1;
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
    n as i32
}

/// Replace the lobby transcript. `utf8` is one line per message as
/// "time\twho\ttext\tsystem\tme", where system and me are "1" or "0".
///
/// The whole transcript is replaced rather than appended to because the
/// hub keeps NO history: the client is the only scrollback that exists, so
/// the caller is already holding the full list and a diffing protocol here
/// would just be a second place for it to go wrong.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_chat(utf8: *const std::os::raw::c_char) {
    let mut lines: Vec<ChatLine> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let mut f = line.split('\t');
                let time = f.next().unwrap_or_default();
                let who = f.next().unwrap_or_default();
                let body = f.next().unwrap_or_default();
                let system = f.next().unwrap_or("0") == "1";
                let me = f.next().unwrap_or("0") == "1";
                lines.push(ChatLine {
                    time: time.into(),
                    who: who.into(),
                    text: body.into(),
                    system,
                    me,
                });
            }
        }
    }
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui
                .set_chat(slint::ModelRc::new(slint::VecModel::from(lines)));
        }
    });
}

/// Drain the library search text into `buf` as UTF-8, returning its byte
/// length, or 0 when it has not changed since the last call. An EMPTY
/// string is still a change -- it means the user cleared the box -- so the
/// return distinguishes "nothing pending" from "pending, and empty" by
/// writing a NUL and returning 0 only in the former case.
///
/// # Safety
/// `buf` must be writable for `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_take_filter(
    buf: *mut std::os::raw::c_char,
    cap: i32,
) -> i32 {
    if buf.is_null() || cap <= 0 {
        return -1;
    }
    let Some(text) = FILTER_OUT.with(|c| c.borrow_mut().take()) else {
        return -1;
    };
    let bytes = text.as_bytes();
    let mut n = bytes.len().min(cap as usize - 1);
    while n > 0 && !text.is_char_boundary(n) {
        n -= 1;
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
    *buf.add(n) = 0;
    n as i32
}

/// A one-line note under the compose box, or "" to clear it. Used for the
/// hub's rate limit, which DROPS the offending line rather than queueing
/// it -- without this the user watches a message they typed never appear.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_chat_notice(utf8: *const std::os::raw::c_char) {
    let text: slint::SharedString = if utf8.is_null() {
        Default::default()
    } else {
        std::ffi::CStr::from_ptr(utf8)
            .to_str()
            .unwrap_or_default()
            .into()
    };
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_chat_notice(text);
        }
    });
}

/// Replace the "looking to play" roster. `utf8` is one player per line as
/// "nick\tstatus\trtt_ms\ttier\tis_self\tvs\topponent\tignored\tblocked" --
/// the fields of the real
/// fm2k::HubUser (FM2K_HubClient.h:32) plus the head-to-head record the
/// launcher already computes, in the order the hub reports them.
///
/// `status` and `tier` are passed through as the hub's own tokens rather
/// than pre-translated into UI states. They are protocol values, and the
/// mapping to a label and a colour belongs in one place -- the .slint --
/// so a hub that starts sending a sixth tier renders plain instead of
/// being silently dropped by a match arm here.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_peers(utf8: *const std::os::raw::c_char) {
    let mut peers: Vec<Peer> = Vec::new();
    let mut orig: i32 = 0;
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let mut f = line.split('\t');
                let nick = f.next().unwrap_or_default();
                let status = f.next().unwrap_or("idle");
                let ping: i32 = f.next().unwrap_or("0").parse().unwrap_or(0);
                let tier = f.next().unwrap_or_default();
                let is_self = f.next().unwrap_or("0") == "1";
                let vs = f.next().unwrap_or("--");
                let opponent = f.next().unwrap_or_default();
                let ignored = f.next().unwrap_or("0") == "1";
                let blocked = f.next().unwrap_or("0") == "1";
                // The bucket is DERIVED, never sent: the caller cannot
                // put a player in AVAILABLE while their status says
                // in_match, so the heading and the row always agree.
                let group = if is_self {
                    0
                } else {
                    match status {
                        "idle" => 1,
                        "challenging" => 2,
                        "in_match" => 3,
                        _ => 4,
                    }
                };
                peers.push(Peer {
                    nick: nick.into(),
                    status: status.into(),
                    ping,
                    tier: tier.into(),
                    is_self,
                    vs: vs.into(),
                    // An opponent name is only meaningful mid-match;
                    // carrying a stale one into an idle row would say
                    // someone is playing when they are free.
                    opponent: if group == 3 { opponent.into() } else { Default::default() },
                    group,
                    ignored,
                    blocked,
                    orig,
                });
                orig += 1;
            }
        }
    }
    // Sort into buckets here, so every caller gets the same ordering and
    // the UI never has to. sort_by_key is stable, so within a bucket the
    // hub's own order survives.
    peers.sort_by_key(|p| p.group);
    let n_avail = peers.iter().filter(|p| p.group == 1).count() as i32;
    let n_challenging = peers.iter().filter(|p| p.group == 2).count() as i32;
    let n_inmatch = peers.iter().filter(|p| p.group == 3).count() as i32;
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_n_avail(n_avail);
            st.ui.set_n_challenging(n_challenging);
            st.ui.set_n_inmatch(n_inmatch);
            st.ui
                .set_peers(slint::ModelRc::new(slint::VecModel::from(peers)));
        }
    });
}

/// Append a games root (UTF-8) and re-evaluate whether setup can proceed.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_add_games_folder(utf8: *const std::os::raw::c_char) {
    if utf8.is_null() {
        return;
    }
    let Ok(path) = std::ffi::CStr::from_ptr(utf8).to_str() else { return };
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return };
        use slint::Model;
        let mut v: Vec<slint::SharedString> = st.ui.get_folders().iter().collect();
        if v.iter().any(|e| e.as_str() == path) {
            return; // adding the same root twice would double every scan
        }
        v.push(path.into());
        st.ui.set_folders(slint::ModelRc::new(slint::VecModel::from(v)));
    });
}

/// Report detected gamepads to the onboarding step (UTF-8, newline-separated).
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_pads(utf8: *const std::os::raw::c_char) {
    let list: Vec<slint::SharedString> = if utf8.is_null() {
        Vec::new()
    } else {
        match std::ffi::CStr::from_ptr(utf8).to_str() {
            Ok(s) if !s.is_empty() => s.lines().map(Into::into).collect(),
            _ => Vec::new(),
        }
    };
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return };
        st.ui.set_pads(slint::ModelRc::new(slint::VecModel::from(list)));
    });
}

/// Replace the game list. `utf8` is one game per line as
/// "title\tcode\tfolder\tengine\tsize\tpeers". Tab-separated rather than JSON because the
/// only field that can contain arbitrary user text is the name, and a tab
/// cannot occur in a Windows filename or a .kgt project title -- so this
/// needs no escaping layer and no parser to get wrong.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_games(utf8: *const std::os::raw::c_char) {
    let mut lib: Vec<LibRow> = Vec::new();
    let mut thumb_paths: Vec<String> = Vec::new();
    let mut extras_all: Vec<Vec<String>> = Vec::new();
    if !utf8.is_null() {
        if let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() {
            for line in text.lines().filter(|l| !l.is_empty()) {
                let mut f = line.split('\t');
                let title = f.next().unwrap_or_default();
                let code = f.next().unwrap_or_default();
                let folder = f.next().unwrap_or_default();
                let engine = f.next().unwrap_or_default();
                let size = f.next().unwrap_or_default();
                let peers: i32 = f.next().unwrap_or("0").parse().unwrap_or(0);
                let thumb_path = f.next().unwrap_or_default();
                let _stem = f.next();
                let _exe = f.next();
                // Field 9: the reviewed slideshow, '|'-joined thumb paths.
                let extras: Vec<String> = f.next().unwrap_or_default()
                    .split('|').filter(|s| !s.is_empty()).map(str::to_string).collect();
                // Art arrives from the worker (images.rs): the rows show
                // up at once and the pictures fill in. A row we have art
                // for already starts WITH it -- rebuilding the library on
                // a resize or a search must not blank every tile.
                let (thumb, rail_thumb, shots) = ART_LAST.with(|m| {
                    m.borrow().get(thumb_path).cloned().unwrap_or_default()
                });
                thumb_paths.push(thumb_path.to_string());
                extras_all.push(extras);
                // `size` is still parsed off the line so the field
                // positions stay put, but it is not carried into the row:
                // the tile has no room for it and an unrendered field is a
                // trap for whoever reads this next.
                let _ = size;
                lib.push(LibRow {
                    title: title.into(),
                    code: code.into(),
                    thumb,
                    rail_thumb,
                    shot1: shots[0].clone(),
                    shot2: shots[1].clone(),
                    shot3: shots[2].clone(),
                    subtitle: folder.into(),
                    engine: engine.into(),
                    peers,
                });
            }
        }
    }
    let lib_len = lib.len();
    // Only a CHANGED set of games needs the worker; a republish of the same
    // library (room counts, a filter, a resize) keeps the art it has.
    let changed = LIB_THUMBS.with(|t| {
        let same = *t.borrow() == thumb_paths;
        *t.borrow_mut() = thumb_paths.clone();
        !same
    }) || LIB_EXTRAS.with(|e| {
        let same = *e.borrow() == extras_all;
        *e.borrow_mut() = extras_all.clone();
        !same
    });
    if changed {
        request_art(thumb_paths);
    }
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_library(slint::ModelRc::new(slint::VecModel::from(lib)));
            // ceil(n/5) row indices -- five tiles per row, width-filling for the grid. Computed here rather
            // than hardcoded in the .slint, where a fixed list silently
            // truncated the library.
            // The sections are published separately by the host; this is
            // only the fallback for a library that arrives before them.
            let _ = lib_len;
        }
    });
}

/// Whether a discovery scan is in flight. Drives the empty-list wording:
/// "no games found" is only TRUE once a scan has finished, and saying it
/// mid-scan is a claim the user will act on.
#[no_mangle]
pub extern "C" fn slint_ui_set_scanning(on: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_scanning(on != 0);
        }
    });
}

/// Status-bar text (scan progress, game count).
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_status(utf8: *const std::os::raw::c_char) {
    if utf8.is_null() {
        return;
    }
    let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() else { return };
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_status(text.into());
        }
    });
}

/// Select a view inside the shell (0 lobby, 1 browse, 2 replays, 3 config).
/// Exists so a view can be opened deterministically for a screenshot or a
/// test, instead of synthesising a click at a screen coordinate and hoping
/// the right window is on top.
#[no_mangle]
pub extern "C" fn slint_ui_set_view(view: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_view(view);
        }
    });
}

/// Drive the match-flow modal. `stage` is 0 none / 1 incoming / 2 outgoing
/// / 3 handshake / 4 booting / 5 post / 6 desync -- a small stable enum on
/// the C++ side rather than a string, so the ABI does not move when the
/// wording does.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_match(
    stage: i32,
    nick: *const std::os::raw::c_char,
    rtt: i32,
    seconds: i32,
    sync_pct: i32,
    detail: *const std::os::raw::c_char,
) {
    let cstr = |p: *const std::os::raw::c_char| -> slint::SharedString {
        if p.is_null() { return Default::default(); }
        std::ffi::CStr::from_ptr(p).to_str().unwrap_or_default().into()
    };
    let st = match stage {
        1 => MatchStage::Incoming,
        2 => MatchStage::Outgoing,
        3 => MatchStage::Handshake,
        4 => MatchStage::Booting,
        5 => MatchStage::Post,
        6 => MatchStage::Desync,
        _ => MatchStage::None,
    };
    STATE.with(|s| {
        let b = s.borrow();
        let Some(sx) = b.as_ref() else { return };
        sx.ui.set_match_stage(st);
        sx.ui.set_match_nick(cstr(nick));
        sx.ui.set_match_rtt(rtt);
        sx.ui.set_match_seconds(seconds);
        sx.ui.set_match_sync(sync_pct);
        sx.ui.set_match_detail(cstr(detail));
    });
}

/// Post-match numbers. Separate from slint_ui_set_match because these only
/// exist once a match has ENDED, and folding them into the stage setter
/// would invite passing stale figures on every earlier stage.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_set_match_result(
    score_self: i32,
    score_opp: i32,
    format: *const std::os::raw::c_char,
    rollbacks: i32,
    avg_rtt: *const std::os::raw::c_char,
    jitter: *const std::os::raw::c_char,
    drops: i32,
    desync: *const std::os::raw::c_char,
    streak: *const std::os::raw::c_char,
) {
    let cstr = |p: *const std::os::raw::c_char| -> slint::SharedString {
        if p.is_null() { return Default::default(); }
        std::ffi::CStr::from_ptr(p).to_str().unwrap_or_default().into()
    };
    STATE.with(|s| {
        let b = s.borrow();
        let Some(sx) = b.as_ref() else { return };
        sx.ui.set_match_score_self(score_self);
        sx.ui.set_match_score_opp(score_opp);
        sx.ui.set_match_format(cstr(format));
        sx.ui.set_match_rollbacks(rollbacks);
        sx.ui.set_match_avg_rtt(cstr(avg_rtt));
        sx.ui.set_match_jitter(cstr(jitter));
        sx.ui.set_match_drops(drops);
        sx.ui.set_match_desync(cstr(desync));
        sx.ui.set_match_streak(cstr(streak));
    });
}

/// Which scene is showing: 0 onboarding, 1 shell.
#[no_mangle]
pub extern "C" fn slint_ui_scene() -> i32 {
    STATE.with(|s| {
        let b = s.borrow();
        b.as_ref().map(|st| st.ui.get_scene()).unwrap_or(-1)
    })
}

/// Jump straight to the shell -- for a returning user whose setup is done.
#[no_mangle]
pub extern "C" fn slint_ui_set_scene(scene: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        if let Some(st) = b.as_ref() {
            st.ui.set_scene(scene);
        }
    });
}

/// Which game row is selected -- proves data crosses the boundary.
#[no_mangle]
pub extern "C" fn slint_ui_selected() -> i32 {
    STATE.with(|s| {
        let b = s.borrow();
        b.as_ref().map(|st| st.ui.get_selected()).unwrap_or(-1)
    })
}
