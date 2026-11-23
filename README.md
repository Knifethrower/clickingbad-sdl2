# Clicking Bad (SDL2)

A native C + SDL2 version of [Clicking Bad](https://github.com/nullism/clickingbad), the incremental web game by
Aaron Meier and contributors. Made for [PortMaster](https://portmaster.games) handhelds; it also runs on desktop Linux.

The game rules follow the original `game.js`. All items, upgrades, achievements, events, purity levels and
risk levels are generated from it (`gen_data.js` writes `data.h`), so the balance is the same as in the browser.
The original repository is the `upstream` submodule, pinned to the commit the data was checked against.

- One C file, SDL2 only: SDL_Renderer for drawing, [stb_truetype](https://github.com/nothings/stb) for text
  (DejaVu Sans Condensed).
- Gamepad (SDL GameController), touch screen, mouse and keyboard.
- Layout fits the screen: stacked on 4:3 and square screens, side by side on wide screens.
- Saves to `clickingbad.sav` in the working directory every 30 seconds, on Start and on quit (also on SIGTERM).
  Browser saves cannot be imported.

## Build

```
git clone --recursive https://github.com/Knifethrower/clickingbad-sdl2.git
cd clickingbad-sdl2
./build.sh
./clickingbad.x86_64
```

Needs `libsdl2-dev`, a C compiler and Node.js. Run it from this folder (it loads `assets/`).
For PortMaster builds, cross-compile against a Debian 11 (bullseye) arm64 sysroot so the binary runs on every
firmware's glibc: `SYSROOT=/path/to/bullseye-arm64 ./build.sh` (clang + lld).

Environment: `CLICKINGBAD_SAVE` sets the save file, `CLICKINGBAD_WINDOW=WxH` opens a window instead of full screen.

## Controls

| Pad | Keyboard | Action |
|--|--|--|
| A | Z, Space | Cook |
| B | X | Sell |
| X | Enter | Buy the selected item, buy an upgrade, choose an option |
| Y | Backspace | Sell the selected item |
| D-Pad Up/Down, left stick | Up/Down, Page Up/Down | Move through the list |
| D-Pad Left/Right, L1/R1 | Left/Right, Q/E, Tab | Switch tab |
| Start | F2 | Save |
| Select + Start | Esc | Save and quit (Esc asks first) |

## License

MIT, like the original game (`LICENSE`). stb_truetype and stb_rect_pack: public domain or MIT (in the headers).
DejaVu fonts: `assets/LICENSE.dejavu.txt`.
