# 2dfm soundtool -- swap sounds in .player/.stage/.demo/.kgt files

Replace a character's sounds **without touching any code block**. FM2K
scripts reference sounds by index into the file's sound table, so swapping
a sound's audio in place is invisible to the game logic -- no script
editing, ever.

Needs: Python 3 (https://python.org, check "Add to PATH" in the installer).
No extra packages. Keep the three files together in one folder:
`soundtool.py`, `kgt.py`, `fm2nd.py`.

## See what's in a file

```
py soundtool.py list Bewear.player
```

Shows every sound slot: index, type (WAV/MIDI), size, format, name.
Most games keep the original filenames as the sound names
(e.g. `bewear_cry.wav`), so finding the one you want is just reading
the list.

## Extract sounds (to listen / edit)

```
py soundtool.py extract Bewear.player -o sounds_out
py soundtool.py extract Bewear.player --sound bewear_cry.wav -o cry.wav
```

## Replace a sound

```
py soundtool.py replace Bewear.player --sound bewear_cry.wav new_cry.wav
py soundtool.py replace Bewear.player --sound 7 new_cry.wav -o Bewear_test.player
```

`--sound` takes the name or the index from `list`. Without `-o` the file
is edited in place and the original is kept as `Bewear.player.bak`.

Your replacement should be a plain PCM WAV, 8 or 16-bit, mono or stereo
(what every editor exports by default). The tool checks the file against
the engine's actual WAV parser before writing, so if it would be silent
or break in-game, you get told why instead of a corrupted character.
After writing it re-verifies that only the one sound changed --
everything else in the file stays byte-for-byte identical.

## Full JSON dump (everything, not just sounds)

```
py fm2nd.py parse Bewear.player -o bewear.json
py fm2nd.py pack  bewear.json Bewear_rebuilt.player
```

The JSON contains every script, sprite frame, palette, and sound
(binary data as base64). `parse` -> edit -> `pack` round-trips
byte-exact. Works on .stage and .demo too; `.kgt` project files use
`kgt.py` with the same commands.
