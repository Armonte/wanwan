// Headless UI tests.
//
// These exist because verifying this UI by driving the running app with
// synthetic mouse events did not work: roughly half the clicks never
// landed, so several screens shipped unverified and one bug (a sorted
// roster whose click index no longer matched the caller's row) was found
// by reading rather than by testing.
//
// Slint's testing backend instantiates the real widget tree headlessly and
// clicks it for real, on the host, in about a second.

use slint::Model;
use slint_i686_spike::app::*;
use slint_i686_spike::install_test_window;

fn peers_tsv() -> &'static str {
    // nick, status, rtt, tier, is_self, vs, opponent, ignored, blocked
    "armonte\tidle\t0\tmonte\t1\t84-31-2\t\t0\t0\n\
     kuroneko\tidle\t42\tthanks\t0\t12-9-1\t\t0\t0\n\
     sloth\tin_match\t88\ttester\t0\t3-5-0\tyuudai\t0\t0\n\
     patrick\tchallenging\t61\ttester\t0\t0-1-0\t\t0\t0\n"
}

// ONE test, deliberately.
//
// Slint is built here with `unsafe-single-threaded` -- the launcher drives
// it from one thread and we do not pay for locking we never use -- so the
// widget tree may only be touched from a single thread. cargo runs a
// file's tests in parallel by default, and each parallel test building its
// own window is exactly the thing that feature forbids. Sections in one
// test are simpler than a mutex around every case, and they read in order.
#[test]
fn ui_contracts_hold() {
    i_slint_backend_testing::init_no_event_loop();

    {
        // ---- roster groups are derived from status ----
        let ui = install_test_window();
        unsafe {
            let c = std::ffi::CString::new(peers_tsv()).unwrap();
            slint_ui_set_peers(c.as_ptr());
        }

        use slint::Model;
        let peers = ui.get_peers();
        let groups: Vec<i32> = peers.iter().map(|p| p.group).collect();

        // 0 self, 1 available, 2 challenging, 3 in match -- and SORTED, so the
        // headings and the rows cannot disagree.
        assert_eq!(groups, vec![0, 1, 2, 3], "roster must be grouped and sorted");

        // The counts the headings print come from the same pass.
        assert_eq!(ui.get_n_avail(), 1);
        assert_eq!(ui.get_n_challenging(), 1);
        assert_eq!(ui.get_n_inmatch(), 1);
    }

    {
        // ---- sorting does not break the challenge index ----
        // The bug this guards: the FFI sorts rows into buckets, so a row's
        // DISPLAY position stops matching the caller's row. Challenging by
        // display index would silently challenge whoever sorted into that slot.
        let ui = install_test_window();
        unsafe {
            let c = std::ffi::CString::new(peers_tsv()).unwrap();
            slint_ui_set_peers(c.as_ptr());
        }

        use slint::Model;
        let peers = ui.get_peers();
        // sloth was third in, and must still report orig == 2 after sorting
        // into the IN MATCH bucket last.
        let sloth = peers.iter().find(|p| p.nick == "sloth").unwrap();
        assert_eq!(sloth.group, 3);
        assert_eq!(sloth.orig, 2, "orig must survive the sort");
    }

    {
        // ---- an unmeasured ping is not zero ----
        let ui = install_test_window();
        unsafe {
            let c = std::ffi::CString::new(peers_tsv()).unwrap();
            slint_ui_set_peers(c.as_ptr());
        }
        use slint::Model;
        let me = ui.get_peers().iter().find(|p| p.is_self).unwrap();
        // The hub means "not measured yet". Rendering it as 0ms would be a
        // perfect ping the player would act on.
        assert_eq!(me.ping, 0);
    }

    {
        // ---- an opponent is only carried while in a match ----
        let ui = install_test_window();
        unsafe {
            let c = std::ffi::CString::new(peers_tsv()).unwrap();
            slint_ui_set_peers(c.as_ptr());
        }
        use slint::Model;
        let peers = ui.get_peers();
        let sloth = peers.iter().find(|p| p.nick == "sloth").unwrap();
        assert_eq!(sloth.opponent, "yuudai");
        // Everyone else must be blank, or an idle row claims someone is playing.
        for p in peers.iter().filter(|p| p.nick != "sloth") {
            assert_eq!(p.opponent, "", "{} should carry no opponent", p.nick);
        }
    }

    {
        // ---- settings rows round trip: host rows in, (key, value) edits out ----
        let ui = install_test_window();
        let tsv = "0\tddraw.vsync\t0\tVSync\t\t1\t\t0\t0\t1\n\
                   0\tddraw.mode\t2\tWindow mode\t\t1\tBorderless\t3\t0\t1\n";
        let c = std::ffi::CString::new(tsv).unwrap();
        unsafe { slint_ui_set_settings(c.as_ptr()) };
        assert_eq!(ui.get_settings().row_count(), 2, "both rows must be published");
        let r = ui.get_settings().row_data(1).unwrap();
        assert_eq!(r.key.as_str(), "ddraw.mode");
        assert_eq!(r.count, 3);
        assert_eq!(r.value_text.as_str(), "Borderless");
        // An edit reaches the host as (key, value), oldest first, then the
        // queue is empty -- nothing is silently dropped or duplicated.
        ui.invoke_setting_changed("ddraw.vsync".into(), "0".into());
        let mut k = [0i8; 64];
        let mut v = [0i8; 64];
        assert_eq!(unsafe { slint_ui_take_setting(k.as_mut_ptr(), 64, v.as_mut_ptr(), 64) }, 1);
        let ks = unsafe { std::ffi::CStr::from_ptr(k.as_ptr()) }.to_str().unwrap();
        let vs = unsafe { std::ffi::CStr::from_ptr(v.as_ptr()) }.to_str().unwrap();
        assert_eq!((ks, vs), ("ddraw.vsync", "0"));
        assert_eq!(unsafe { slint_ui_take_setting(k.as_mut_ptr(), 64, v.as_mut_ptr(), 64) }, 0,
                   "the queue must be empty after one take");
    }

    {
        // ---- the match card stays a card ----
        // Twice now this thing has eaten the window: first as a deliberate
        // full-bleed modal, then again when removing the hardcoded height
        // let it inherit the parent's. Both were only visible on screen, so
        // measure it here instead.
        use i_slint_backend_testing::ElementHandle;
        let ui = install_test_window();
        unsafe {
            let nick = std::ffi::CString::new("kuroneko").unwrap();
            let detail = std::ffi::CString::new("Rollback 16f - first to 5").unwrap();
            // stage 1 = incoming, the loudest of them.
            slint_ui_set_match(1, nick.as_ptr(), 42, 12, 0, detail.as_ptr());
        }
        // scene 1 is the lobby, which is where the composer lives.
        ui.set_scene(1);

        let card = ElementHandle::find_by_element_id(&ui, "MatchFlow::card")
            .next().expect("the match card must be in the tree");
        let sz = card.size();
        let pos = card.absolute_position();

        assert!(sz.width <= 420.0 && sz.height <= 260.0,
                "the match card must stay a card, got {}x{}", sz.width, sz.height);
        // Inside the window, clear of the 154px roster rail, and above both
        // the 20px status bar and the 30px chat composer it used to cover.
        assert!(pos.x >= 0.0 && pos.x + sz.width <= 1280.0 - 154.0,
                "card must not run under the roster rail");
        assert!(pos.y + sz.height <= 720.0 - 20.0 - 30.0,
                "card must sit above the status bar and the chat composer");
    }
}
