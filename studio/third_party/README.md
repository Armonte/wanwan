# studio/third_party -- vendored single-header audio decoders

Fetched verbatim with curl on 2026-07-15. Do not edit in place; to update,
re-fetch from upstream and bump the commit hashes here.

## dr_libs (github.com/mackron/dr_libs)
Commit: `6d78776c2c05358e351e3c67878a8b681c76c5d1` (master)
License: public domain / MIT-0 (dual, see file tails)

| file      | version | URL |
|-----------|---------|-----|
| dr_wav.h  | v0.14.6 | https://raw.githubusercontent.com/mackron/dr_libs/6d78776c2c05358e351e3c67878a8b681c76c5d1/dr_wav.h |
| dr_mp3.h  | v0.7.4  | https://raw.githubusercontent.com/mackron/dr_libs/6d78776c2c05358e351e3c67878a8b681c76c5d1/dr_mp3.h |
| dr_flac.h | v0.13.4 | https://raw.githubusercontent.com/mackron/dr_libs/6d78776c2c05358e351e3c67878a8b681c76c5d1/dr_flac.h |

## stb (github.com/nothings/stb)
Commit: `31c1ad37456438565541f4919958214b6e762fb4` (master)
License: public domain / MIT (dual, see file tail)

| file         | version | URL |
|--------------|---------|-----|
| stb_vorbis.c | v1.22   | https://raw.githubusercontent.com/nothings/stb/31c1ad37456438565541f4919958214b6e762fb4/stb_vorbis.c |

Consumer: `studio/core/audio/audio_convert.cpp` (defines the DR_*_IMPLEMENTATION
macros and includes stb_vorbis.c in pull/pushdata-free whole-file mode there;
nothing else in the tree should include these with implementations enabled).
