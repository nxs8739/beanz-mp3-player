# BEANZ MP3 Player — v1.0

**A lightweight MP3 music player for the Nintendo GameCube.**

Browse folders on SD storage, choose a song, and listen using a custom purple GameCube-themed interface. Designed for straightforward single-track playback.

## Features

- MP3 decoding with minimp3 and stereo audio playback through libogc ASND
- SD2SP2 and SD Gecko file browsing with mount-root navigation protection
- Now Playing display with scrolling filenames and elapsed / total song time
- A to pause/resume, B to stop or go back, and START to exit
- Automatic return to the file browser when a song finishes
- Retry handling for temporary ASND audio-buffer queue rejections
- Finished Now Playing and File Browser wallpapers

The player deliberately omits skipping, seeking, repeat, shuffle, playlists, and automatic next-track playback.

## Build

Requires devkitPPC, libogc 3.2.1, and libfat. From the project directory:

```bash
make clean && make
```

The build produces `beanz-mp3-player.dol` for launching through Swiss or another GameCube homebrew loader.

## License

Original application source: MIT. Bundled minimp3 decoder: CC0 1.0. See `LICENSE` and `THIRD_PARTY_NOTICES.md` for full details. Unofficial homebrew project; not affiliated with or endorsed by Nintendo.

**Simple by design. Reliable by implementation!**
