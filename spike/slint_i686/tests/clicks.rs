// Click tests, in their own binary.
//
// These need the integration backend and a running event loop, which
// cannot coexist in a process with the no-event-loop backend the property
// tests use -- separate test binaries mean separate processes.
//
// They are ONE test on purpose: the backend initialises once per process,
// and the event loop runs once. Splitting them into several #[test]s would
// race on both.

use i_slint_backend_testing::ElementHandle;
use slint::platform::PointerEventButton;
use slint_i686_spike::app::*;
use slint_i686_spike::install_test_window;

fn peers_tsv() -> &'static str {
    "armonte\tidle\t0\tmonte\t1\t84-31-2\t\t0\t0\n\
     kuroneko\tidle\t42\tthanks\t0\t12-9-1\t\t0\t0\n\
     sloth\tin_match\t88\ttester\t0\t3-5-0\tyuudai\t0\t0\n\
     patrick\tchallenging\t61\ttester\t0\t0-1-0\t\t0\t0\n"
}

#[test]
fn controls_respond_to_real_clicks() {
    i_slint_backend_testing::init_integration_test_with_system_time();
    slint::spawn_local(async move {
        let ui = install_test_window();
        // Scene 1 is the shell. Scene 0 is onboarding, and it is the
        // default -- with it, none of the shell's elements are
        // instantiated and every query comes back empty.
        ui.set_scene(1);
        // ComponentHandle::show brings the tree up so conditionals
        // instantiate; without it every query sees an empty window.
        use slint::ComponentHandle;
        ui.show().unwrap();

        // ---- the replay source toggle ----
        //
        // This is the control that synthetic mouse events aimed at the
        // running window could never reliably hit, so it shipped
        // unverified. Here it is clicked for real.
        ui.set_view(2);
        let tabs: Vec<ElementHandle> =
            ElementHandle::find_by_element_id(&ui, "Replays::srcta").collect();
        assert_eq!(tabs.len(), 2, "THIS PC and EVERYONE");

        assert_eq!(ui.get_replay_source(), 0);
        tabs[1].single_click(PointerEventButton::Left).await;
        assert_eq!(ui.get_replay_source(), 1, "clicking EVERYONE selects it");
        assert_eq!(slint_ui_take_replay_source(), 1,
                   "and the host must be told, or the list never changes");

        tabs[0].single_click(PointerEventButton::Left).await;
        assert_eq!(ui.get_replay_source(), 0);
        assert_eq!(slint_ui_take_replay_source(), 0);

        // ---- roster: single click does nothing, double challenges ----
        //
        // A roster is read far more often than acted on, so a stray click
        // while scanning names must not challenge anyone.
        ui.set_view(0);
        unsafe {
            let c = std::ffi::CString::new(peers_tsv()).unwrap();
            slint_ui_set_peers(c.as_ptr());
        }
        let rows: Vec<ElementHandle> =
            ElementHandle::find_by_element_id(&ui, "RosterRow::touch").collect();
        assert!(!rows.is_empty(), "roster rows must exist");

        let _ = slint_ui_take_challenge();
        rows[1].single_click(PointerEventButton::Left).await;
        assert_eq!(slint_ui_take_challenge(), -1,
                   "a single click must not challenge");

        rows[1].double_click(PointerEventButton::Left).await;
        // Row 1 after sorting is kuroneko, whose ORIGINAL index is 1.
        assert_eq!(slint_ui_take_challenge(), 1,
                   "a double click challenges that row's original index");

        slint::quit_event_loop().unwrap();
    })
    .unwrap();
    slint::run_event_loop().unwrap();
}
