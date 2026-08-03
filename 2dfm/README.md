# 2dfm/ -- format reference, NOT built

C++ parsers for the 2DFM on-disk formats (`.player`, `.stage`, `.demo`,
pictures, palettes, common resources). Nothing compiles these -- they are not
in any `CMakeLists.txt`.

They are still load-bearing as **documentation**: the Python tooling mirrors
this parse logic and cites it by file and line, e.g.
`tools/dump_player_pics.py:3` ("Mirrors the parse logic in
2dfm/2dfmFileReader.cpp") and again at line 135 for `readCommonResourcePart`.
`tools/extractors/extract_2dfm_audio.py` follows the same structures.

So: if you change how a format is parsed in the Python tools, check these
files still describe the same thing, or update them. If they ever stop matching
reality they become worse than useless, since the tools' comments point here.

The live C++ parser used by the shipping build is
`launcher/game/FM2K_KgtParser.cpp`; the studio's is `studio/core/kgt_file.cpp`.
Those are the ones to edit if you want behavior to change.

Accompanying notes: `2dfm_binary_analysis.md`,
`2dfm_player_repo_analysis.md`, `unity_vs_cpp_comparison.md`.
