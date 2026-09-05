// Slint's DEFAULT resource mode is AsAbsolutePath, which its own docs
// describe as "only useful for debugging, since the files must still be
// present at the same path on the machine running the application". That
// is a build-machine path baked into a shipped exe: it works on the
// developer's box and breaks on every user's. EmbedFiles puts the font
// bytes in the binary instead.
//
// Credit to the PovertyCaster session for flagging this before it cost us
// a release; confirmed by reading slint-build 1.17.1's own enum docs
// rather than taken on faith.
fn main() {
    // ELEMENT INTROSPECTION FOR TESTS. Without this the testing backend
    // finds an empty widget tree -- every query returns zero, which looks
    // like a broken UI rather than a missing build flag. Debug profile
    // only, so the shipped binary carries none of it.
    if std::env::var("PROFILE").as_deref() == Ok("debug") {
        println!("cargo:rustc-env=SLINT_EMIT_DEBUG_INFO=1");
        std::env::set_var("SLINT_EMIT_DEBUG_INFO", "1");
    }

    let cfg = slint_build::CompilerConfiguration::new()
        .embed_resources(slint_build::EmbedResourcesKind::EmbedFiles);
    slint_build::compile_with_config("ui/app.slint", cfg).expect("slint compile failed");
}
