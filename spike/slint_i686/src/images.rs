// images.rs -- the library art pipeline.
//
// THE RULE: nothing rasterizes game art per frame on the CPU. The window
// renders with FemtoVG on the GPU (lib.rs GlWindow): each capture is
// decoded ONCE here, becomes one Slint image, and the GPU scales that
// texture to whatever the tile is. Nothing is re-encoded on resize and
// nothing "fills in" tile by tile after a maximize. (The software renderer
// walked every image pixel through a per-pixel blend loop: 40 ms per
// scrolled frame at 2560 wide. That is why the renderer changed.)
//
// SAMPLING, though, is still ours to get right. The renderer minifies with
// plain bilinear and no mipmaps, and a 640x480 frame drawn at three
// quarters size shimmers as it scrolls -- two taps cannot represent four
// source pixels. So every tile is ALSO prefiltered here, once per tile
// size, with Lanczos3 to exactly the pixels it will occupy, and the GPU
// then samples it 1:1. The full frame stands in until that arrives, and
// the rail chip (70x52) is prefiltered the same way.
use std::collections::HashMap;
use std::sync::{Arc, Mutex, OnceLock};

pub type Rgba = image::RgbaImage;

struct Job {
    kind: u8,           // 0 library row, 1 review gallery entry
    row: usize,
    thumb_path: String, // 320x240 capture (rail chip source)
    full_path: String,  // 640x480 capture (GPU tile source), may not exist
    extras: Vec<String>,// approved slideshow shots (thumb paths)
    tile: (u32, u32),
    rail: (u32, u32),
    gen: u64,
}

pub struct Done {
    pub kind: u8,               // 0 library row, 1 review gallery entry
    pub row: usize,
    pub path: String,           // the thumb path, the art's identity
    pub tile: Option<Rgba>,     // Lanczos-prefiltered to the tile's exact size
    pub extras: Vec<Rgba>,      // slideshow shots, prefiltered the same way
    pub rail: Option<Rgba>,     // rail chip
    pub full: Option<Arc<Rgba>>,// the frame, for the GAME header
    pub gen: u64,
}

struct Shared {
    cache: Mutex<HashMap<String, Arc<Rgba>>>,
    done: Mutex<Vec<Done>>,
}

fn shared() -> &'static Shared {
    static S: OnceLock<Shared> = OnceLock::new();
    S.get_or_init(|| Shared { cache: Mutex::new(HashMap::new()), done: Mutex::new(Vec::new()) })
}

fn decode(path: &str) -> Option<Arc<Rgba>> {
    if path.is_empty() {
        return None;
    }
    if let Some(hit) = shared().cache.lock().ok()?.get(path) {
        return Some(hit.clone());
    }
    let img = image::ImageReader::open(path).ok()?.with_guessed_format().ok()?.decode().ok()?;
    let rgba = Arc::new(img.to_rgba8());
    shared().cache.lock().ok()?.insert(path.to_string(), rgba.clone());
    Some(rgba)
}

fn fit(src: &Rgba, w: u32, h: u32) -> Rgba {
    if w == 0 || h == 0 {
        return Rgba::new(1, 1);
    }
    if src.width() == w && src.height() == h {
        return src.clone();
    }
    image::imageops::resize(src, w, h, image::imageops::FilterType::Lanczos3)
}

/// Queue art for these rows. Older generations' results are dropped on
/// arrival.
pub fn request(rows: Vec<(usize, String, Vec<String>)>, tile: (u32, u32), rail: (u32, u32), gen: u64) {
    let jobs: Vec<Job> = rows
        .into_iter()
        .filter(|(_, p, _)| !p.is_empty())
        .map(|(row, thumb_path, extras)| {
            // The index names the 320x240 ".thumb.webp"; the 640x480 frame
            // sits beside it under the same name without ".thumb".
            let full_path = thumb_path.replacen(".thumb.", ".", 1);
            Job { kind: 0, row, thumb_path, full_path, extras, tile, rail, gen }
        })
        .collect();
    if jobs.is_empty() {
        return;
    }
    // A small pool: decoding is the whole cost of a library's first paint,
    // one 640x480 webp at a time took a quarter of a minute for a hundred
    // games and the wall filled in one tile at a time. Each worker takes
    // the next job off a shared queue so no thread finishes early.
    let mut jobs = jobs;
    jobs.reverse();
    let queue = Arc::new(Mutex::new(jobs));
    let workers = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2).clamp(2, 6);
    for _ in 0..workers {
        let queue = queue.clone();
        std::thread::spawn(move || loop {
            let Some(j) = queue.lock().ok().and_then(|mut q| q.pop()) else { break };
            let full = if std::path::Path::new(&j.full_path).exists() { decode(&j.full_path) } else { None };
            let thumb = decode(&j.thumb_path);
            let big = full.as_ref().or(thumb.as_ref());
            let small = thumb.as_ref().or(full.as_ref());
            // Each approved shot: its full frame when it exists, else the thumb.
            let extras: Vec<Rgba> = j.extras.iter().filter_map(|t| {
                let f = t.replacen(".thumb.", ".", 1);
                let src = if std::path::Path::new(&f).exists() { decode(&f) } else { decode(t) };
                src.map(|s| fit(&s, j.tile.0, j.tile.1))
            }).collect();
            let d = Done {
                kind: j.kind,
                row: j.row,
                path: j.thumb_path.clone(),
                tile: big.map(|s| fit(s, j.tile.0, j.tile.1)),
                extras,
                rail: small.map(|s| fit(s, j.rail.0, j.rail.1)),
                full: big.cloned(),
                gen: j.gen,
            };
            if let Ok(mut q) = shared().done.lock() {
                q.push(d);
            }
        });
    }
}

/// The review gallery: each capture's thumb, fitted to the gallery card.
/// Same worker, a different kind, so the library's generation gate does
/// not swallow these.
pub fn request_gallery(paths: Vec<String>, card: (u32, u32), gen: u64) {
    let jobs: Vec<Job> = paths
        .into_iter()
        .enumerate()
        .filter(|(_, p)| !p.is_empty())
        .map(|(row, thumb_path)| Job {
            kind: 1, row, full_path: thumb_path.clone(), thumb_path, extras: Vec::new(), tile: card, rail: (1, 1), gen,
        })
        .collect();
    if jobs.is_empty() {
        return;
    }
    std::thread::spawn(move || {
        for j in jobs {
            let src = decode(&j.thumb_path);
            let d = Done {
                kind: 1,
                row: j.row,
                path: j.thumb_path.clone(),
                tile: src.as_ref().map(|s| fit(s, j.tile.0, j.tile.1)),
                extras: Vec::new(),
                rail: None,
                full: None,
                gen: j.gen,
            };
            if let Ok(mut q) = shared().done.lock() {
                q.push(d);
            }
        }
    });
}

/// Take everything the worker finished. Called on the UI thread.
pub fn take_done() -> Vec<Done> {
    shared().done.lock().map(|mut q| std::mem::take(&mut *q)).unwrap_or_default()
}

pub fn to_slint(img: &Rgba) -> slint::Image {
    let buf = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::clone_from_slice(
        img.as_raw(),
        img.width(),
        img.height(),
    );
    slint::Image::from_rgba8(buf)
}
