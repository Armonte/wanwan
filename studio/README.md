# 2dfm Studio -- sounds-first GUI editor for FM2K files

Design doc: `docs/dev/2dfm_studio_design.md` (read first).
Oracle parsers: `tools/kgt/{kgt,fm2nd,blocks}.py` (byte-exact, corpus-proven).

Layout:
- `core/`   kgtcore: parser/serializer (`kgt_file.h`), sound xref
  (`xref.h`), audio decode/convert (`core/audio/`). C++17, no UI deps.
- `app/`    ImGui + SDL3 shell (mingw single .exe, vendored deps).
- `tests/`  native x86_64 corpus gate + unit tests (build like
  `tools/fpk_core/build_native.sh`).
- `third_party/` vendored single-header audio decoders (dr_libs, stb_vorbis).

Safety contract: `Serialize(Parse(x)) == x` for every FM2K file, enforced
by the corpus gate over every local game file before the GUI may write.
