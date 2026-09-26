# Arrange

Four audio tracks of clips on a bar timeline — the audio song mode.

Clips come from the Sequencer's **BOUNCE**: a scene is played once and its pads
are written to a WAV in the open project, placed after the last block of the
chosen track. Here they are arranged: moved along the grid, duplicated,
deleted, muted per track, and played back as one "Song" channel in the Mixer.

A project is a folder on the card, `/sdcard/SONGS/<name>/` with `song.json`
and `clips/*.wav`, so clips bounced from different kits sit in one song. The
song has one tempo (the first bounce sets it); a clip made at another tempo
plays at the speed it was recorded.

| File | What |
|---|---|
| `src/ArrangeApp.cpp` | timeline, transport, clip tools, project picker |

The engine behind it is `crosspad::ISongEngine` (crosspad-core), reached
through `PlatformServices::songEngine`; the ESP-IDF implementation is
`components/crosspad-platform-idf/SongEnginePort.cpp`.
