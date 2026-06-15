# zstd-singlefile -- decompress-only amalgamation

Single-file Zstandard **decompressor** (zstd 1.5.7), used by the FM2KHook .fpk
virtual-file reconstructor (`FM2KHook/src/vfs/fpk_reader.cpp`).

- `zstddeclib.c` -- the amalgamated decompress-only library (one TU; it inlines
  its own copy of `zstd.h`/`zstd_errors.h` internally, so it is fully
  self-contained). Compiled directly into FM2KHook + FM95Hook, the same way the
  SafetyHook/Zydis/miniz single-file amalgamations are. No build system, no
  cross-sysroot dependency -- the i686-w64-mingw32 toolchain has NO zstd, so
  vendoring the decompressor is the only way to ship `ZSTD_decompress` /
  `ZSTD_getFrameContentSize` in the DLL.
- `zstd.h`, `zstd_errors.h` -- the stock public headers, on the include path so
  consumers (`fpk_reader.cpp`) can `#include <zstd.h>`.

Provenance: zstd 1.5.7 build/single_file_libs amalgamation
(`python combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c
zstddeclib-in.c`). BSD/GPLv2 dual-licensed (BSD used here).

Decompress-only is sufficient: the .fpk format is produced by the Python packer
(`tools/kgt/fpk.py`); the DLL only ever inflates. If compression is ever needed
in-process, swap in the full single-file lib (`zstd.c`).
