# Third-Party Notices

This product (the FM2K Rollback launcher and `FM2KHook`/`FM95Hook` DLLs) bundles
and links the third-party components below. Each is the property of its
respective authors and is used under its own license -- the project's own LICENSE
(PolyForm Noncommercial + Revocation Addendum) does NOT apply to them. The full
license text for each lives in its directory under `vendored/`.

All bundled components are under permissive (non-copyleft) licenses; none impose
copyleft obligations on this project's own code.

| Component | Use | License | Text |
|---|---|---|---|
| SDL3 | windowing / input / audio / timing | zlib | `vendored/SDL/LICENSE.txt` |
| SDL_image | image loading (icon, UI) | zlib | `vendored/SDL_image/LICENSE.txt` |
| SDL_net | networking helpers | zlib | `vendored/SDL_net/LICENSE.txt` |
| Dear ImGui | launcher + in-game overlay UI | MIT | `vendored/imgui/LICENSE.txt` |
| GekkoNet | rollback netcode session library | BSD-2-Clause | `vendored/GekkoNet/LICENSE` |
| MinHook | x86 function hooking | BSD-2-Clause | `vendored/minhook/LICENSE.txt` |
| miniupnpc | UPnP-IGD port mapping | BSD-3-Clause | `vendored/miniupnp/LICENSE` |
| quill | async logging | MIT | `vendored/quill/LICENSE` |
| SafetyHook | mid-function hooking | Boost Software License 1.0 | `vendored/safetyhook/` |
| Zydis | x86 disassembler (used by SafetyHook) | MIT | `vendored/zydis-amalgamated/` |
| doctest | unit-test framework (tests only) | MIT | `vendored/doctest/` |
| miniz | in-process zip extraction | MIT / public domain | `vendored/miniz/` |
| stb | header-only image/util helpers | MIT / public domain | `vendored/stb/` |
| xxHash | fast hashing (game-content hash) | BSD-2-Clause | `vendored/xxhash/` |
| zstd | compression (asset pipeline) | BSD-3-Clause (used under the BSD option) | `vendored/zstd-singlefile/` |
| Opus | audio codec | BSD-3-Clause | `vendored/opus-i686/` |
| libpng / zlib / libjpeg / libtiff | image codecs inside SDL_image | libpng / zlib / IJG / libtiff (all permissive) | `vendored/SDL_image/external/` |

## Slint (planned native frontend -- NOT YET SHIPPED)

> Status: the UI currently ships on Dear ImGui (listed above). There is no
> Slint code in the tree yet. This section is kept so the terms are already
> settled when the frontend lands; **drop the "planned" wording the moment a
> build actually links Slint**, and do not treat it as a current dependency
> before then.

When it ships, the native frontend will use [Slint](https://slint.dev) under the
**Slint Royalty-free Desktop, Mobile, and Web Applications License** (granted to
Armonte Williams). Slint's own copyright and license notices are retained in its
source. Attribution is provided per that license; see the project's download
page. Slint is NOT covered by this project's LICENSE.

## Notes

- `vendored/SDL/src/hidapi/LICENSE-gpl3.txt` is one option of hidapi's
  multi-license offering; SDL bundles hidapi under its permissive option, so no
  GPL terms apply to this product.
- The SDL_image AVIF/JXL/WEBP codecs (`libavif`, `libjxl`, `aom`, `dav1d`,
  `libwebp`) are NOT built or linked; their nested example/contrib licenses do
  not apply to anything in the shipped binaries.
