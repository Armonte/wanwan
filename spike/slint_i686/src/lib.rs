// C ABI over a Slint UI rendered into a caller-owned pixel buffer.
//
// THE ARRANGEMENT, and why it is this way round: SDL3 (C++ side) owns the
// window, the event loop and presentation; Slint owns only the widget tree
// and draws into memory when asked. Slint's normal backend wants to own the
// window itself through winit, which would mean two window systems in one
// process and no path to the SDL3 setup the launcher already uses for input
// and rendering. MinimalSoftwareWindow is the supported way to invert that.
//
// It is also the arrangement that survives the open questions: a software
// renderer needs no GPU driver (so it works under a remote desktop and on
// the ancient hardware some of these games run on), and it is the same
// shape a native Linux build would use.
#![allow(clippy::missing_safety_doc)]

use slint::platform::software_renderer::PremultipliedRgbaColor;
use std::cell::RefCell;
use std::rc::Rc;

slint::include_modules!();

// The app-data half of the C ABI. Split out when this file reached the
// repo's 1000-line ceiling: the seam is by concern, not line count --
// below is the window, its surface and its input devices; `app` is
// everything the launcher puts INTO that window and drains back out.
// Public so the headless test suite can call the C ABI directly. The
// linker does not care, and a test that cannot reach the functions it is
// testing is not a test.
pub mod app;
pub mod images;
// The drain slots live in `app`, but the callbacks that fill them are
// registered here, on the window that owns them.
use app::{
    BROWSE_PENDING, CHALLENGE_PENDING, CHAT_OUT, CONFIG_DIRTY, FILTER_OUT, INPUT_OUT, LEAVE_OUT, SETTINGS_OUT,
    CONNECT_HUB, LAUNCH_PENDING, REPLAY_SOURCE, WINDOW_ACTION, MATCH_ACTION, ONBOARD_DONE, ROSTER_ACTION,
    WATCH_OUT,
};

struct SdlPlatform {
    window: Rc<dyn slint::platform::WindowAdapter>,
}

// ── The GPU window: FemtoVG over SDL3's OpenGL context ──
//
// Why not the software renderer for the real window: it rasterizes every
// pixel of every image through a per-pixel blend loop on the CPU, which
// measured at 40 ms per scrolled frame for a wall of 4:3 captures at
// 2560 wide. FemtoVG draws the same tree with the GPU, images become
// textures uploaded once, and a maximized frame is a few hundred
// microseconds of driver time. SDL3 owns the window and the GL context;
// Slint only ever sees three C callbacks.
type GlMakeCurrentFn = unsafe extern "C" fn(*mut std::ffi::c_void) -> i32;
type GlSwapFn = unsafe extern "C" fn(*mut std::ffi::c_void);
type GlGetProcFn =
    unsafe extern "C" fn(*mut std::ffi::c_void, *const std::os::raw::c_char) -> *const std::ffi::c_void;

struct SdlGl {
    make_current: GlMakeCurrentFn,
    swap: GlSwapFn,
    get_proc: GlGetProcFn,
    user: *mut std::ffi::c_void,
}

unsafe impl slint::platform::femtovg_renderer::OpenGLInterface for SdlGl {
    fn ensure_current(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        // SAFETY: the host registered these callbacks with a pointer that
        // outlives the window (the SDL_Window itself).
        if unsafe { (self.make_current)(self.user) } != 0 {
            return Err("SDL_GL_MakeCurrent failed".into());
        }
        Ok(())
    }
    fn swap_buffers(&self) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        unsafe { (self.swap)(self.user) };
        Ok(())
    }
    fn resize(
        &self,
        _w: std::num::NonZeroU32,
        _h: std::num::NonZeroU32,
    ) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        Ok(())
    }
    fn get_proc_address(&self, name: &std::ffi::CStr) -> *const std::ffi::c_void {
        unsafe { (self.get_proc)(self.user, name.as_ptr()) }
    }
}

pub(crate) struct GlWindow {
    window: slint::Window,
    renderer: slint::platform::femtovg_renderer::FemtoVGRenderer,
    size: std::cell::Cell<slint::PhysicalSize>,
    needs_redraw: std::cell::Cell<bool>,
}

impl GlWindow {
    fn new(gl: SdlGl) -> Result<Rc<Self>, slint::PlatformError> {
        let renderer = slint::platform::femtovg_renderer::FemtoVGRenderer::new(gl)?;
        Ok(Rc::new_cyclic(|w: &std::rc::Weak<Self>| Self {
            window: slint::Window::new(w.clone()),
            renderer,
            size: Default::default(),
            needs_redraw: std::cell::Cell::new(true),
        }))
    }
    /// Render when something changed. Returns true when a frame was drawn
    /// (and swapped, so this is where the vsync wait lands).
    fn draw_if_needed(&self) -> bool {
        // An animation (a wheel step's 180 ms glide, a hover fade) only
        // produces frames if we keep drawing while it runs: nothing sets
        // the redraw flag between its ticks.
        let animating = self.window.has_active_animations();
        if !self.needs_redraw.replace(false) && !animating {
            return false;
        }
        if let Err(e) = self.renderer.render() {
            eprintln!("slint: GL render failed: {e}");
        }
        true
    }
}

impl slint::platform::WindowAdapter for GlWindow {
    fn window(&self) -> &slint::Window {
        &self.window
    }
    fn renderer(&self) -> &dyn slint::platform::Renderer {
        &self.renderer
    }
    fn size(&self) -> slint::PhysicalSize {
        self.size.get()
    }
    fn set_size(&self, size: slint::WindowSize) {
        let sf = self.window.scale_factor();
        self.size.set(size.to_physical(sf));
        self.window.dispatch_event(slint::platform::WindowEvent::Resized {
            size: size.to_logical(sf),
        });
        self.needs_redraw.set(true);
    }
    fn request_redraw(&self) {
        self.needs_redraw.set(true);
    }
}

impl slint::platform::Platform for SdlPlatform {
    fn create_window_adapter(
        &self,
    ) -> Result<Rc<dyn slint::platform::WindowAdapter>, slint::PlatformError> {
        Ok(self.window.clone())
    }
}

thread_local! {
    pub(crate) static STATE: RefCell<Option<AppState>> = RefCell::new(None);
}

pub(crate) struct AppState {
    // Exactly one of these is Some: the software window (tests, the
    // screenshot tool, --bench) or the GPU window (the launcher).
    sw: Option<Rc<slint::platform::software_renderer::MinimalSoftwareWindow>>,
    gl: Option<Rc<GlWindow>>,
    pub(crate) ui: AppWindow,
}

/// Create the UI. Call once, before any render. Returns 0 on success.
#[no_mangle]
pub extern "C" fn slint_ui_init(width: u32, height: u32) -> i32 {
    let window = slint::platform::software_renderer::MinimalSoftwareWindow::new(
        // RepaintBufferType::ReusedBuffer: the host keeps ONE buffer and
        // hands the same one back every frame (host.cpp `buf`, reallocated
        // only on resize, when Slint repaints everything anyway). Slint
        // then repaints only the dirty region and reports it, and the host
        // uploads only that rectangle. NewBuffer repainted every pixel of
        // a maximized window for a one-row change -- 3.6 million pixels
        // and a 14 MB upload per frame at 2560x1440 -- which is what made
        // scrolling and maximize lurch.
        slint::platform::software_renderer::RepaintBufferType::ReusedBuffer,
    );
    if slint::platform::set_platform(Box::new(SdlPlatform {
        window: window.clone(),
    }))
    .is_err()
    {
        return -1; // already initialized
    }
    window.set_size(slint::PhysicalSize::new(width, height));

    let ui = match AppWindow::new() {
        Ok(u) => u,
        Err(_) => return -2,
    };
    wire_callbacks(&ui);

    STATE.with(|s| *s.borrow_mut() = Some(AppState { sw: Some(window), gl: None, ui }));
    0
}

/// Create the UI on SDL3's OpenGL context. The three callbacks make the
/// context current, swap the window, and resolve GL entry points
/// (SDL_GL_MakeCurrent / SDL_GL_SwapWindow / SDL_GL_GetProcAddress); `user`
/// is handed back to each. Returns 0 on success.
///
/// # Safety
/// The callbacks and `user` must stay valid for the life of the window.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_init_gl(
    width: u32,
    height: u32,
    make_current: GlMakeCurrentFn,
    swap: GlSwapFn,
    get_proc: GlGetProcFn,
    user: *mut std::ffi::c_void,
) -> i32 {
    let window = match GlWindow::new(SdlGl { make_current, swap, get_proc, user }) {
        Ok(w) => w,
        Err(e) => {
            eprintln!("slint: FemtoVG over SDL GL failed: {e}");
            return -3;
        }
    };
    let adapter: Rc<dyn slint::platform::WindowAdapter> = window.clone();
    if slint::platform::set_platform(Box::new(SdlPlatform { window: adapter })).is_err() {
        return -1; // already initialized
    }
    window.window.set_size(slint::PhysicalSize::new(width, height));

    let ui = match AppWindow::new() {
        Ok(u) => u,
        Err(_) => return -2,
    };
    wire_callbacks(&ui);

    STATE.with(|s| *s.borrow_mut() = Some(AppState { sw: None, gl: Some(window), ui }));
    0
}

/// Draw the GPU window if anything changed. Returns 1 when a frame was
/// rendered and swapped (the vsync wait is inside), 0 when idle.
#[no_mangle]
pub extern "C" fn slint_ui_render_gl() -> i32 {
    slint::platform::update_timers_and_animations();
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return 0 };
        let Some(gl) = st.gl.as_ref() else { return 0 };
        i32::from(gl.draw_if_needed())
    })
}


/// Register every UI callback.
///
/// Shared by `slint_ui_init` and the test window on purpose. When the test
/// setup wired nothing, a click correctly flipped the property and the
/// host was never told -- which is a real bug shape, and the test would
/// have reported the UI broken instead of the harness incomplete.
fn wire_callbacks(ui: &AppWindow) {
    ui.on_browse_folder(|| BROWSE_PENDING.with(|f| f.set(true)));
    ui.on_onboarding_done(|| ONBOARD_DONE.with(|f| f.set(true)));
    ui.on_challenge(|i| CHALLENGE_PENDING.with(|f| f.set(i + 1)));
    ui.on_spectate(|i| ROSTER_ACTION.with(|f| f.set((i << 4) | 1)));
    ui.on_ignore_user(|i| ROSTER_ACTION.with(|f| f.set((i << 4) | 2)));
    ui.on_block_user(|i| ROSTER_ACTION.with(|f| f.set((i << 4) | 3)));
    // Every one of these was declared and drawn but registered nowhere, so
    // the match modal could not even be dismissed from its own buttons --
    // a modal with no exit is worse than no modal.
    ui.on_launch(|| LAUNCH_PENDING.with(|f| f.set(true)));
    ui.on_leave_room(|i| LEAVE_OUT.with(|f| f.set(i + 1)));
    ui.on_match_accept(|| MATCH_ACTION.with(|f| f.set(1)));
    ui.on_match_decline(|| MATCH_ACTION.with(|f| f.set(2)));
    ui.on_match_cancel(|| MATCH_ACTION.with(|f| f.set(3)));
    ui.on_match_rematch(|| MATCH_ACTION.with(|f| f.set(4)));
    ui.on_match_quit(|| MATCH_ACTION.with(|f| f.set(5)));
    ui.on_match_end(|| MATCH_ACTION.with(|f| f.set(9)));
    ui.on_send_chat(|t| CHAT_OUT.with(|c| *c.borrow_mut() = Some(t.to_string())));
    ui.on_filter_games(|t| FILTER_OUT.with(|c| *c.borrow_mut() = Some(t.to_string())));
    ui.on_config_changed(|| CONFIG_DIRTY.with(|f| f.set(true)));
    ui.on_input_action(|a| INPUT_OUT.with(|q| q.borrow_mut().push_back(a.to_string())));
    ui.on_setting_changed(|k, v| {
        SETTINGS_OUT.with(|q| q.borrow_mut().push_back((k.to_string(), v.to_string())))
    });
    ui.on_connect_hub(|| CONNECT_HUB.with(|f| f.set(true)));
    ui.on_window_action(|a| WINDOW_ACTION.with(|f| f.set(a)));
    ui.on_replay_source_changed(|s| REPLAY_SOURCE.with(|f| f.set(s)));
    ui.on_watch_replay(|p, seek| {
        WATCH_OUT.with(|w| *w.borrow_mut() = Some((p.to_string(), seek)))
    });

}

/// Build a window and register it, for the headless test backend.
///
/// Tests cannot call `slint_ui_init`: it installs OUR platform, and the
/// testing backend has already installed its own. Everything after that
/// point is identical though -- the same AppWindow, in the same STATE the
/// C ABI reads -- so the setters under test are the shipping ones rather
/// than a parallel arrangement built to be testable.
#[doc(hidden)]
pub fn install_test_window() -> AppWindow {
    let ui = AppWindow::new().expect("AppWindow");
    let window = slint::platform::software_renderer::MinimalSoftwareWindow::new(
        slint::platform::software_renderer::RepaintBufferType::NewBuffer,
    );
    wire_callbacks(&ui);
    STATE.with(|s| {
        *s.borrow_mut() = Some(AppState { sw: Some(window), gl: None, ui: ui.clone_strong() })
    });
    ui
}

/// Render into `pixels` (RGBA8888, `stride_px` pixels per row).
/// Returns 1 if anything was drawn this frame, 0 if nothing changed.
///
/// The 0/1 return is not decoration: Slint only redraws when something
/// actually changed, so the caller can skip the texture upload and the
/// present entirely on an idle frame. A launcher sitting on a game list is
/// idle almost all the time, and this is what keeps it from burning a core
/// to redraw an unchanged list at vsync.
///
/// # Safety
/// `pixels` must point to at least `stride_px * height` u32s, writable, and
/// must be THE SAME buffer as the previous call (ReusedBuffer): Slint only
/// repaints what changed. After a resize the caller's new buffer is fine
/// because the resize marks the whole window dirty. `out_rect` (x, y, w, h,
/// may be null) receives the repainted rectangle when the call returns 1.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_render(
    pixels: *mut u32,
    width: u32,
    height: u32,
    stride_px: u32,
    out_rect: *mut i32,
) -> i32 {
    if pixels.is_null() || stride_px < width || height == 0 {
        return 0;
    }
    slint::platform::update_timers_and_animations();
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return 0 };
        let mut rect = [0i32, 0, width as i32, height as i32];
        let Some(sw) = st.sw.as_ref() else { return 0 };
        if st.ui.window().has_active_animations() {
            st.ui.window().request_redraw();
        }
        let drawn = sw.draw_if_needed(|renderer| {
            let slice = std::slice::from_raw_parts_mut(
                pixels as *mut PremultipliedRgbaColor,
                (stride_px as usize) * (height as usize),
            );
            let region = renderer.render(slice, stride_px as usize);
            let o = region.bounding_box_origin();
            let z = region.bounding_box_size();
            rect = [o.x as i32, o.y as i32, z.width as i32, z.height as i32];
        });
        if drawn && !out_rect.is_null() {
            std::ptr::copy_nonoverlapping(rect.as_ptr(), out_rect, 4);
        }
        i32::from(drawn)
    })
}

/// Tell Slint the display scale, and resize to `phys_w` x `phys_h` PHYSICAL
/// pixels. Call on startup and on every resize / DPI change.
///
/// RASTERIZE AT PHYSICAL SIZE, ALWAYS. The tempting shortcut is to render
/// at logical size and let the GPU upscale the result; that is what makes
/// text blurry on a scaled display, because glyphs get hinted for the
/// smaller grid and then stretched. Handing Slint the scale factor keeps
/// layout in logical coordinates while rasterizing glyphs at output
/// resolution, which is the only arrangement where text is actually sharp.
/// Credit for this one goes to the PovertyCaster session, which shipped the
/// blurry version first and measured the fix.
///
/// The consequence for the caller: the pixel buffer must be sized in
/// PHYSICAL pixels, and the render benchmark that matters is the largest
/// physical size you will ever rasterize -- not the window's logical size.
#[no_mangle]
pub extern "C" fn slint_ui_set_scale(phys_w: u32, phys_h: u32, scale: f32) {
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return };
        if scale > 0.0 {
            st.ui.window().dispatch_event(slint::platform::WindowEvent::ScaleFactorChanged {
                    scale_factor: scale,
                });
        }
        st.ui.window().set_size(slint::PhysicalSize::new(phys_w, phys_h));
        // Force a full repaint: a resize invalidates everything, and the
        // caller's buffer is a different size than the one the last frame
        // was drawn into.
        st.ui.window().request_redraw();
    });
}

/// Feed a scroll wheel delta (logical pixels).
///
/// Was simply missing: pointer motion and buttons were forwarded but the
/// wheel was not, so every scrollable surface was mouse-dead while looking
/// perfectly normal.
#[no_mangle]
pub extern "C" fn slint_ui_scroll(x: f32, y: f32, dx: f32, dy: f32) {
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return };
        let position = slint::LogicalPosition::new(x, y);
        // Establish hover FIRST. Slint routes a scroll to whatever the
        // pointer is over, and if no move has been delivered at this
        // position the event lands on nothing. A wheel event can arrive
        // without a preceding move -- a wheel tilt, a trackpad gesture, or
        // simply a mouse that has not moved since the last click -- so the
        // scroll carries its own position rather than assuming one.
        st.ui.window().dispatch_event(slint::platform::WindowEvent::PointerMoved { position });
        // NOT WindowEvent::PointerScrolled. The public platform event
        // reaches the Flickable with phase `Cancelled`, whose branch sets
        // the viewport outright -- no glide. The 180 ms wheel animation
        // (Slint PR #10846, issue #4115) runs only for a wheel event with
        // phase `Moved`, which only the internal event type can carry.
        // Same call the winit backend makes.
        use i_slint_core::input::{MouseEvent, TouchPhase};
        i_slint_core::window::WindowInner::from_pub(st.ui.window()).process_mouse_input(
            MouseEvent::Wheel {
                position: i_slint_core::lengths::LogicalPoint::new(x, y),
                delta_x: dx,
                delta_y: dy,
                phase: TouchPhase::Moved,
            },
        );
    });
}

/// Feed a UTF-8 text-input event (SDL_EVENT_TEXT_INPUT). This is the path
/// that carries IME output, so it is what makes Japanese entry work at all
/// -- keycodes alone cannot express it.
#[no_mangle]
pub unsafe extern "C" fn slint_ui_text(utf8: *const std::os::raw::c_char) {
    if utf8.is_null() {
        return;
    }
    let Ok(text) = std::ffi::CStr::from_ptr(utf8).to_str() else { return };
    if text.is_empty() {
        return;
    }
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return };
        st.ui.window().dispatch_event(slint::platform::WindowEvent::KeyPressed {
            text: text.into(),
        });
        st.ui.window().dispatch_event(slint::platform::WindowEvent::KeyReleased {
            text: text.into(),
        });
    });
}

/// Feed a non-text key (backspace, enter, arrows, tab, escape, home/end).
/// `code` is the small enum below, NOT an SDL keycode -- keeping the
/// mapping on the C++ side means this ABI does not change when SDL does.
///  1 Backspace  2 Return  3 Tab  4 Escape
///  5 Left  6 Right  7 Up  8 Down  9 Home  10 End  11 Delete
#[no_mangle]
pub extern "C" fn slint_ui_key(code: i32, pressed: i32) {
    use slint::platform::Key;
    let k = match code {
        1 => Key::Backspace,
        2 => Key::Return,
        3 => Key::Tab,
        4 => Key::Escape,
        5 => Key::LeftArrow,
        6 => Key::RightArrow,
        7 => Key::UpArrow,
        8 => Key::DownArrow,
        9 => Key::Home,
        10 => Key::End,
        11 => Key::Delete,
        _ => return,
    };
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return };
        let text: slint::SharedString = k.into();
        st.ui.window().dispatch_event(if pressed != 0 {
            slint::platform::WindowEvent::KeyPressed { text }
        } else {
            slint::platform::WindowEvent::KeyReleased { text }
        });
    });
}

/// Feed a pointer move/press/release from SDL3.
///
/// `button` is SDL's index: 1 left, 2 middle, 3 right. It has to be
/// carried through rather than assumed: reporting every press as Left is
/// what made right-click menus in the UI unreachable, since the handler
/// tests the button and a forged Left never matches.
#[no_mangle]
pub extern "C" fn slint_ui_pointer(x: f32, y: f32, pressed: i32, released: i32, button: i32) {
    STATE.with(|s| {
        let b = s.borrow();
        let Some(st) = b.as_ref() else { return };
        let pos = slint::LogicalPosition::new(x, y);
        let btn = match button {
            2 => slint::platform::PointerEventButton::Middle,
            3 => slint::platform::PointerEventButton::Right,
            _ => slint::platform::PointerEventButton::Left,
        };
        if pressed != 0 {
            st.ui.window().dispatch_event(slint::platform::WindowEvent::PointerPressed {
                position: pos,
                button: btn,
            });
        } else if released != 0 {
            st.ui.window().dispatch_event(slint::platform::WindowEvent::PointerReleased {
                position: pos,
                button: btn,
            });
        } else {
            st.ui.window().dispatch_event(slint::platform::WindowEvent::PointerMoved { position: pos });
        }
    });
}

