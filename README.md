# The Housekeeper

A Space Invaders variant in C17 + SDL2. One source file, no assets, identical
native and WebAssembly builds.

Burgers drip fat, martinis spill, and the carpet suffers. You are the
housekeeper with a spray bottle and four mid-century credenzas for cover.

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

`docs/` is what GitHub Pages serves. `./deploy.sh` rebuilds, commits, pushes
and enables Pages in one go.

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
| `Space` | fire |
| `P` | pause |
| `M` | mute |
| `F` | fullscreen |
| `Esc` | back to menu / quit |
| `1`–`4`, `↑` `↓`, `Enter` | difficulty menu |

## Rules

Win by destroying all 40 invaders. Lose when the soil meter fills (30
droppings on the carpet), you run out of lives (3), or an invader reaches the
floor. Martinis are worth 20, burgers 10. The grid speeds up as it thins.

| Level | Shots in flight | Reload | Grid step |
|-------|-----------------|--------|-----------|
| Easy | 3 | none | 1200 ms |
| Medium | 2 | 250 ms | 900 ms |
| Hard | 1 | 600 ms | 900 ms |
| Nightmare | 1 | 1200 ms | 650 ms |

## How it stays portable

All state lives in one global struct and the frame is a no-argument
`main_loop()`. Native code calls it in a `while` loop; Emscripten hands it to
`emscripten_set_main_loop`. That `#ifdef` in `main()` is the only one in the
file. Sprites are SDL2 primitives, the font is a 5×7 bitmap table, and the
sound effects are square waves and noise bursts synthesised at startup and
played through `SDL_QueueAudio`, so nothing needs the filesystem on either
target.

## Files

```
housekeeper.c    the game
housekeeper.6    man page
shell.html       Emscripten HTML shell (keyboard focus, fullscreen, no page scroll)
Makefile         native / install / wasm / serve
deploy.sh        push to GitHub and publish docs/ via Pages
docs/            WASM build output (generated)
```
