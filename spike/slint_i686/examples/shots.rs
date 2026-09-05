// shots -- render every screen of the launcher to a file, headlessly.
//
// This uses the SOFTWARE render path: slint_ui_init installs the software
// platform and slint_ui_render draws into a caller-owned buffer. No window,
// no SDL, no Windows -- so the whole launcher can be photographed on the
// host in a couple of seconds and reviewed away from the machine it runs
// on. The launcher itself no longer renders this way: its window is
// FemtoVG on SDL3's OpenGL context (lib.rs GlWindow), which needs a real
// window and so cannot run headless. The widget tree and its layout are
// the same either way, which is what makes these shots worth reviewing;
// what they cannot show is GPU-side image filtering.
//
// It is not the testing backend: that one owns its own window, so our
// render entry point would draw an empty one.
//
// cargo run --example shots -- <outdir>

use slint_i686_spike::app::*;
use slint_i686_spike::*;

const W: u32 = 1280;
const H: u32 = 720;

fn cs(s: &str) -> std::ffi::CString {
    std::ffi::CString::new(s).unwrap()
}

/// 24-bit BMP, bottom-up. Written by hand rather than pulling an encoder
/// into the shipping dependency list for a debug tool.
fn write_bmp(path: &str, px: &[u32], w: u32, h: u32) {
    let row = ((w * 3 + 3) / 4) * 4;
    let size = 54 + row * h;
    let mut o: Vec<u8> = Vec::with_capacity(size as usize);
    o.extend_from_slice(b"BM");
    o.extend_from_slice(&size.to_le_bytes());
    o.extend_from_slice(&0u32.to_le_bytes());
    o.extend_from_slice(&54u32.to_le_bytes());
    o.extend_from_slice(&40u32.to_le_bytes());
    o.extend_from_slice(&(w as i32).to_le_bytes());
    o.extend_from_slice(&(h as i32).to_le_bytes());
    o.extend_from_slice(&1u16.to_le_bytes());
    o.extend_from_slice(&24u16.to_le_bytes());
    o.extend_from_slice(&[0u8; 24]);
    for y in (0..h).rev() {
        let mut n = 0;
        for x in 0..w {
            let p = px[(y * w + x) as usize];
            // The render buffer is RGBA8888, so the low byte is RED --
            // BMP wants BGR. Writing it the other way round turned every
            // orange accent blue, which looked like a theme change rather
            // than a byte order.
            o.push(((p >> 16) & 0xFF) as u8);   // B
            o.push(((p >> 8) & 0xFF) as u8);    // G
            o.push((p & 0xFF) as u8);           // R
            n += 3;
        }
        while n < row { o.push(0); n += 1; }
    }
    std::fs::write(path, o).expect("write bmp");
}

// The section builder lives in the C++ host, so the example reproduces the
// same shape rather than linking it: enough for a screenshot, and clearly
// marked as a stand-in.
fn slint_i686_spike_browse_rows(tsv: &str, mru: &[String]) -> String {
    let lines: Vec<&str> = tsv.lines().filter(|l| !l.is_empty()).collect();
    let peers: Vec<i32> = lines.iter()
        .map(|l| l.split('\t').nth(5).and_then(|v| v.parse().ok()).unwrap_or(0))
        .collect();
    let titles: Vec<&str> = lines.iter().map(|l| l.split('\t').next().unwrap_or("")).collect();
    let n = lines.len();
    let mut out = String::new();
    let mut head = |o: &mut String, h: &str, e: &str| {
        o.push_str(&format!("{h}\t{e}\t0\t-1\t-1\t-1\t-1\t-1\n"));
    };
    let tiles = |o: &mut String, idx: &[usize], dim: bool| {
        for c in idx.chunks(5) {
            o.push_str(&format!("\t\t{}", if dim { 1 } else { 0 }));
            for k in 0..5 {
                o.push('\t');
                o.push_str(&c.get(k).map(|v| v.to_string()).unwrap_or_else(|| "-1".into()));
            }
            o.push('\n');
        }
    };
    let mut pop: Vec<usize> = (0..n).filter(|&i| peers[i] > 0).collect();
    pop.sort_by_key(|&i| -peers[i]);
    pop.truncate(10);
    if !pop.is_empty() {
        head(&mut out, "POPULAR NOW", &format!("live -- {} of {}", pop.len(), n));
        tiles(&mut out, &pop, false);
    }
    let recent: Vec<usize> = mru.iter()
        .filter_map(|t| titles.iter().position(|x| x == t)).take(5).collect();
    if !recent.is_empty() {
        head(&mut out, "RECENT", "you, most recent first");
        tiles(&mut out, &recent, false);
    }
    let quiet: Vec<usize> = (0..n).filter(|&i| peers[i] == 0).take(10).collect();
    if quiet.len() > 4 {
        head(&mut out, "HIDDEN GEMS", "seed 0x9E37");
        tiles(&mut out, &quiet, true);
    }
    let all: Vec<usize> = (0..n).collect();
    head(&mut out, "ALL INSTALLED", &format!("{n} games"));
    tiles(&mut out, &all, false);
    out
}

fn shoot(dir: &str, name: &str) {
    let mut buf = vec![0u32; (W * H) as usize];
    // Two passes: the first lays out and may leave partial state on a tree
    // that only just changed, the second draws it settled.
    unsafe {
        slint_ui_render(buf.as_mut_ptr(), W, H, W, std::ptr::null_mut());
        slint_ui_render(buf.as_mut_ptr(), W, H, W, std::ptr::null_mut());
    }
    write_bmp(&format!("{dir}/{name}.bmp"), &buf, W, H);
    println!("  {name}");
}

fn seed_library() {
    // Real rows from the capture sweep when it is present, so the shots
    // show the actual art rather than empty tiles. Paths in the index are
    // Windows-side; this runs on the host, so they are translated.
    let mut tsv = String::new();
    if let Ok(idx) = std::fs::read_to_string("/mnt/c/games/captures/index.tsv") {
        for (i, line) in idx.lines().take(40).enumerate() {
            let mut f = line.split('\t');
            let exe = f.next().unwrap_or("");
            let thumb = f.next().unwrap_or("");
            let stem = exe.rsplit('\\').next().unwrap_or("game").trim_end_matches(".exe");
            let host_thumb = thumb.replacen("C:\\", "/mnt/c/", 1).replace('\\', "/");
            tsv += &format!("{stem}\t{}\tfolder\tFM2K\t2.1 MB\t{}\t{host_thumb}\t{stem}\n",
                            &stem.chars().take(3).collect::<String>().to_uppercase(),
                            if i % 3 == 0 { 2 } else { 0 });
        }
    }
    if tsv.is_empty() {
        for i in 0..12 {
            tsv += &format!("Sample Fighting Game {i}\tSF{i}\t\tFM2K\t2.1 MB\t0\t\tsample{i}\n");
        }
    }
    unsafe { slint_ui_set_games(cs(&tsv).as_ptr()) };
    // Rail chips carry the real titles, split the way the host splits them,
    // so the shots show what the rail actually looks like.
    let mut rail = String::new();
    for (i, line) in tsv.lines().take(8).enumerate() {
        let title = line.split('\t').next().unwrap_or("");
        let (a, b) = title.split_at(title.len().min(11));
        rail += &format!("{i}\t{a}\t{}\n", b.trim());
    }
    unsafe { slint_ui_set_rail(cs(&rail).as_ptr()) };

    // The browse body is SECTIONED by the host, so a shot that only sets
    // the library renders an empty grid -- which is what the first pass
    // did, and it looked like a broken view rather than a missing call.
    let titles: Vec<String> =
        tsv.lines().map(|l| l.split('\t').next().unwrap_or("").to_string()).collect();
    let mru: Vec<String> = titles.iter().take(4).cloned().collect();
    let rows = slint_i686_spike_browse_rows(&tsv, &mru);
    unsafe { slint_ui_set_browse_rows(cs(&rows).as_ptr()) };
}

fn seed_hub() {
    let peers = "armonte\tidle\t0\tmonte\t1\t84-31-2\t\t0\t0\n\
                 kuroneko\tidle\t42\tthanks\t0\t12-9-1\t\t0\t0\n\
                 hanako\tidle\t140\tguest\t0\t--\t\t0\t0\n\
                 anon_1874\tidle\t23\t\t0\t--\t\t0\t0\n\
                 patrick\tchallenging\t61\ttester\t0\t0-1-0\t\t0\t0\n\
                 sloth\tin_match\t88\ttester\t0\t3-5-0\tyuudai\t0\t0\n\
                 yuudai\tin_match\t54\t\t0\t--\tsloth\t0\t0\n";
    let chat = "02:31\thub\tkuroneko joined channel\t1\t0\n\
                02:33\tkuroneko\tanyone up for a few games\t0\t0\n\
                02:34\thanako\tgive me 10min\t0\t0\n\
                02:36\tarmonte\tim around\t0\t1\n\
                02:41\thub\tsloth vs yuudai -- room #6F2A -- 18ms\t1\t0\n\
                02:44\tpatrick\tgl hf\t0\t0\n";
    unsafe {
        slint_ui_set_peers(cs(peers).as_ptr());
        slint_ui_set_chat(cs(chat).as_ptr());
        slint_ui_set_status(cs("108 GAME(S)").as_ptr());
    }
    slint_ui_set_connected(1);
}

fn seed_replays() {
    // Counts match the rows listed, and the third column carries the SIDE
    // on a match row (the host puts it there once the game is named by the
    // set above). A fixture that disagrees with itself teaches the reviewer
    // to distrust the screenshot instead of the code.
    let rows = "SET\t2026-08-26 22:13\twanwan\tarmonte\tkuroneko\t2 matches\t35 KB\t/x.fm2kset\t/x.fm2kset\t0\t\t\t1\n\
                MATCH\t22:11\tP0\tarmonte\tkuroneko\t2-1\t13 KB\t/a\t/x.fm2kset\t1\t\t\t1\n\
                MATCH\t22:12\tP0\tarmonte\tkuroneko\t0-2\t12 KB\t/b\t/x.fm2kset\t2\t\t\t2\n\
                SET\t2026-08-26 18:17\twanwan\tHOSTBOT\tGUESTBOT\t1 match\t13 KB\t/y.fm2kset\t/y.fm2kset\t0\t\t\t2\n\
                MATCH\t18:17\tP1\tHOSTBOT\tGUESTBOT\t1-0\t12 KB\t/c\t/y.fm2kset\t1\t\t\t1\n";
    unsafe { slint_ui_set_replays(cs(rows).as_ptr()) };
}

fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| "screens".into());
    std::fs::create_dir_all(&dir).expect("mkdir");

    assert_eq!(slint_ui_init(W, H), 0, "init");
    slint_ui_set_scale(W, H, 1.0);
    seed_library();
    seed_hub();
    seed_replays();
    unsafe { slint_ui_set_display_name(cs("armonte").as_ptr()) };

    slint_ui_set_scene(0);
    shoot(&dir, "01-onboarding");

    slint_ui_set_scene(1);
    slint_ui_set_view(0);
    shoot(&dir, "02-lobby");

    slint_ui_set_view(1);
    shoot(&dir, "03-browse");

    slint_ui_set_view(2);
    slint_ui_set_replay_source(0);
    shoot(&dir, "04-replays-this-pc");
    slint_ui_set_replay_source(1);
    unsafe { slint_ui_set_replay_note(cs("loading...").as_ptr()) };
    shoot(&dir, "05-replays-everyone");
    slint_ui_set_replay_source(0);
    unsafe { slint_ui_set_replay_note(cs("").as_ptr()) };

    // Config rows as the host publishes them (host_settings.cpp builds the
    // real ones from the launcher's stores; this is a representative slice
    // in the same TSV so the tabs photograph with content, not blank).
    let rows = "0\t\t6\tEdits apply to the next game launch.\tEditing C:\\games\\cnc-ddraw\\ddraw.ini\t\t\t0\t0\t1\n\
0\t\t7\tWindow\t\t\t\t0\t0\t1\n\
0\tddraw.mode\t2\tWindow mode\tBorderless is a windowed fullscreen; Fullscreen is exclusive upscaled.\t1\tBorderless (windowed fullscreen)\t3\t0\t1\n\
0\tddraw.width\t1\tWidth\t0 = use the game's own resolution.\t0\t\t0\t0\t1\n\
0\tddraw.maintas\t0\tKeep aspect ratio\t\t1\t\t0\t0\t1\n\
0\tddraw.boxing\t0\tInteger scaling\tPixel-exact multiples; leaves black borders.\t1\t\t0\t0\t1\n\
0\t\t7\tScaling and sync\t\t\t\t0\t0\t1\n\
0\tddraw.d3d9_filter\t2\tScaling filter\tBicubic and Lanczos need a 16 or 32-bit game.\t0\tNearest neighbour\t4\t0\t1\n\
0\tddraw.vsync\t0\tVSync\tOff can lower input latency at the cost of tearing.\t0\t\t0\t0\t1\n\
0\t\t6\tRenderer: direct3d9  (managed by the launcher)\tThe in-game overlay needs Direct3D 9, so the launcher pins it once per start.\t\t\t0\t0\t1\n\
0\t\t7\tAlt+Enter and hotkeys\t\t\t\t0\t0\t1\n\
0\tddraw.keytogglefullscreen\t4\tToggle fullscreen (Alt + key)\tVirtual-key code, hex. 0x00 = unbound.\t0x0D\t0x0D\t0\t0\t1\n\
0\t\t9\tcnc-ddraw advanced settings (rarely needed)\t\t\t\t0\t0\t1\n\
0\tddraw.maxfps\t1\tmaxfps\t-1 = screen, 0 = unlimited, n = cap.\t0\t\t0\t1\t1\n\
1\t\t7\tSOCD, per player, per device\t\t\t\t0\t0\t1\n\
1\tinput.p0.socd\t2\tPlayer 1 device\t\t2\t1 -- Hitbox SOCD (L+R neutral, U wins U+D)\t7\t0\t1\n\
1\tinput.p1.socd\t2\tPlayer 2 device\t\t0\tInherit the global default\t7\t0\t1\n\
1\tinput.binder\t5\tOpen the binder\tNot implemented here yet.\t\t\t0\t0\t1\n\
2\t\t7\tGame audio\t\t\t\t0\t0\t1\n\
2\taudio.bgm_muted\t0\tMute music\tThe running game picks this up within a second.\t0\t\t0\t0\t1\n\
3\t\t7\tInput delay\t\t\t\t0\t0\t1\n\
3\tsettings.delay_mode\t2\tDelay\tComputed modes size the delay to your ping.\t0\tcomputed (avg ping)\t19\t0\t1\n\
4\t\t7\tLauncher\t\t\t\t0\t0\t1\n\
4\tdev.update_channel\t2\tRelease channel\t\t0\tStable\t3\t0\t1\n\
4\tdev.auto_upload_logs\t0\tSend crash and desync diagnostics\tPersonal data is scrubbed first. Default on.\t1\t\t0\t0\t1\n\
5\t\t6\tWonderfulWorld_ver_0946\tMatch settings write game_configs\\WonderfulWorld_ver_0946.ini\t\t\t0\t0\t1\n\
5\tcfg.vs_single_play\t1\tRounds to win (1v1)\t1-9\t-1\t\t0\t0\t1\n\
5\tpatch.random_stage_enable\t0\tRandom stage each match\t\t0\t\t0\t0\t1\n";
    unsafe { slint_ui_set_settings(cs(rows).as_ptr()) };
    // The binder, as host_binder.cpp publishes it: one card per player.
    let players = "0\tKeyboard\t-1\t0\t2\tHitbox: L+R = neutral, U wins U+D\t0\tWonderfulWorld_ver_0946\t\n\
1\tXbox Controller\t0\t1\t0\tLauncher default\t0\tWonderfulWorld_ver_0946\tPause / Start\n";
    let irows = "0\t2\tUp\tUp\t\t0\t1\n0\t3\tDown\tDown\t\t0\t1\n0\t0\tLeft\tLeft\t\t0\t1\n0\t1\tRight\tRight\t\t0\t1\n\
0\t4\tA\tZ\t\t0\t1\n0\t5\tB\tX\t\t1\t1\n0\t6\tC\tC\t\t0\t1\n0\t7\tD\tA\t\t0\t1\n0\t8\tE\tS\t\t0\t1\n0\t9\tF\tD\t\t0\t1\n0\t10\tPause / Start\tReturn\t\t0\t1\n\
1\t2\tUp\tPad: dpup\tStick: lefty -\t0\t1\n1\t3\tDown\tPad: dpdown\tStick: lefty +\t0\t1\n1\t0\tLeft\tPad: dpleft\tStick: leftx -\t0\t1\n1\t1\tRight\tPad: dpright\tStick: leftx +\t0\t1\n\
1\t4\tA\tPad: x\t\t0\t1\n1\t5\tB\tPad: y\t\t0\t1\n1\t6\tC\tPad: rightshoulder\t\t0\t1\n1\t7\tD\tPad: a\t\t0\t1\n1\t8\tE\tPad: b\t\t0\t1\n1\t9\tF\tPad: righttrigger\t\t0\t1\n1\t10\tPause / Start\t\t\t0\t1\n";
    unsafe {
        slint_ui_set_input_players(cs(players).as_ptr());
        slint_ui_set_input_rows(cs(irows).as_ptr());
    }
    slint_ui_set_view(3);
    for (i, n) in ["06-config-display", "07-config-input", "08-config-network",
                   "09-config-audio", "10-config-hub", "10b-config-game"].iter().enumerate() {
        slint_ui_set_config_tab(i as i32);
        shoot(&dir, n);
    }

    slint_ui_set_view(0);
    let stages = [(1, "11-match-incoming"), (2, "12-match-outgoing"),
                  (3, "13-match-handshake"), (4, "14-match-booting"),
                  (5, "15-match-post"), (6, "16-match-desync")];
    for (stage, name) in stages {
        unsafe {
            slint_ui_set_match(stage, cs("kuroneko").as_ptr(), 42, 12, 96,
                               cs("Rollback 16f - stage random - first to 5").as_ptr());
            slint_ui_set_match_result(3, 2, cs("BO5").as_ptr(), 142,
                                      cs("42.4ms").as_ptr(), cs("+/-0.6ms").as_ptr(),
                                      0, cs("none").as_ptr(), cs("+6").as_ptr());
        }
        shoot(&dir, name);
    }
    unsafe { slint_ui_set_match(0, cs("").as_ptr(), 0, 0, 0, cs("").as_ptr()) };

    // set_ingame carries no nick -- the card reads root.match_nick, which
    // the host has set by the time a session is live. Seed it, or the shot
    // shows "--" for an opponent the running build would have named.
    unsafe {
        slint_ui_set_match(4, cs("kuroneko").as_ptr(), 42, 0, 100,
                           cs("").as_ptr());
    }
    slint_ui_set_ingame(1, 3, 5, 214, 42, 142, 0);
    shoot(&dir, "17-ingame");
    slint_ui_set_ingame(0, 0, 0, 0, 0, 0, 0);
    unsafe { slint_ui_set_match(0, cs("").as_ptr(), 0, 0, 0, cs("").as_ptr()); }

    slint_ui_set_connected(0);
    unsafe { slint_ui_set_peers(cs("").as_ptr()); slint_ui_set_chat(cs("").as_ptr()) };
    shoot(&dir, "18-lobby-signed-out");

    println!("done -> {dir}");
}
