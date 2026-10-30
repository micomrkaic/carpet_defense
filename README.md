# The Housekeeper

A Space Invaders variant in C17 + SDL2. One source file, no assets, identical
native and WebAssembly builds.

Burgers drip fat, martinis spill, a Bloody Mary drifts past dropping tomato
juice, and the carpet suffers. You are the housekeeper with a spray bottle, a
mop, and four mid-century credenzas for cover.

**Play in the browser:** https://micomrkaic.github.io/carpet_defense/

## Build

```sh
sudo apt install libsdl2-dev       # Debian/Ubuntu/Pop!_OS
make                               # -> ./housekeeper
make install                       # -> ~/.local/bin/housekeeper + man page
man housekeeper
```

`make install PREFIX=/usr/local` for a system-wide install.

### WebAssembly

```sh
source ~/emsdk/emsdk_env.sh        # Emscripten on PATH
make wasm                          # -> docs/index.{html,js,wasm}
make serve                         # http://localhost:8000
```

`docs/` is what GitHub Pages serves. `./deploy.sh "message"` rebuilds,
commits, pushes and (first time) enables Pages.

## Usage

```
housekeeper [-d N] [-f] [-m]
  -d, --difficulty N   start at level 1..4, skip the menu
  -f, --fullscreen
  -m, --mute
```

| Key | Action |
|-----|--------|
| `←` `→` / `A` `D` | move |
| `Space` | fire (hold to autofire) |
| hold `↓` / `S` | mop the stain you are standing over |
| `P` | pause |
| `M` | mute |
| `F` | fullscreen |
| `Esc` | back to menu / quit |
| `1`–`4`, `↑` `↓`, `Enter` | difficulty menu |

Gamepads work (stick/D-pad, A fire, B back, X pause, Y mute, Start enter).
The browser build shows touch buttons on phones and tablets.

## Rules

Clear all 40 invaders to finish a wave. The furniture is reset and you gain
a life (max 5); the carpet is **not** cleaned, and the next wave is 10%
faster with one more dropping in the air. You lose when the soil meter fills
(30 stains), you run out of lives, or an invader reaches the floor.

| | Points |
|---|---|
| Martini | 20 |
| Burger | 10 |
| Mopped stain | 5 |
| Bloody Mary | 50 / 100 / 150 |

Best scores per difficulty are kept in `~/.local/state/housekeeper/hiscore`
(natively) or `localStorage` (browser).

| Level | Shots in flight | Reload | Grid step |
|-------|-----------------|--------|-----------|
| Easy | 3 | none | 1200 ms |
| Medium | 2 | 250 ms | 900 ms |
| Hard | 1 | 600 ms | 900 ms |
| Nightmare | 1 | 1200 ms | 650 ms |

## How it stays portable

All state lives in one global struct and the frame is a no-argument
`main_loop()`. Native code calls it in a `while` loop; Emscripten hands it to
`emscripten_set_main_loop`. The only other platform split is the pair of
ten-line hi-score functions (a file vs. `localStorage`). Sprites are SDL2
primitives, the font is a 5×7 bitmap table, and the sound effects are square
waves and noise bursts synthesised at startup and played through
`SDL_QueueAudio`, so nothing needs the filesystem on either target.

Two Emscripten 6.x lessons baked into the build: a `SDL_WINDOW_RESIZABLE`
window takes its size from the canvas's CSS box, so `shell.html` gives it
one; and `ALLOW_MEMORY_GROWTH=1` now produces a resizable `ArrayBuffer`
that `TextDecoder` rejects during WebGL init, so the heap is a fixed 64 MB.

## Files

```
housekeeper.c    the game
housekeeper.6    man page
shell.html       Emscripten HTML shell (focus, fullscreen, touch controls)
Makefile         native / install / wasm / serve
deploy.sh        push to GitHub and publish docs/ via Pages
docs/            WASM build output (generated)
```
