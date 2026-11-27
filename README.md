# RELIC

**RELIC** by **Nerve Axis** won the Amiga demo competition at Assembly 1998 in
Helsinki, and went to Aminet on 11 August 1998 as `nvxrelic`. It wants a 68020,
the AGA chipset, a PAL display and four megabytes of fast RAM, and by its
authors' own count it is **33,495 lines of hand-written assembly**.

This repository is a reconstruction of it in ANSI C - not an emulator, and not a
capture. Every part here is a transcription of the original's own code, reading
the 1998 executable and its data file at run time: the meshes, the textures, the
sine tables, the camera splines and the music are all taken out of those bytes
as the demo starts. Where the demo hard-codes a number, so does this; where it
builds a table, this builds the same table.

It is also the write-up. About half the work is in `docs/` - 23,000 lines on how
the effects actually function, what was measured against real hardware, and what
is still unknown.

---

## What it runs

Twenty-one parts, in the demo's own order:

> a terminal typing itself, a sun, a logo, the approach to a vessel, a portal,
> a window over a planet, the orb forming, the ship beneath it, an explosion,
> the release, the Earth, sand, the temple, the return, debris, a valley, a
> city burning, the planet destroyed, and the ark leaving.

The clock is the music's, not the frame counter's. Scenes end on a position in
the module rather than after a count of fields, which is how the cuts land where
they did in 1998.

Two things are deliberately **not** faithful, and both are written down:

- **Resolution.** The Amiga renders 160x100 chunky pixels and encodes them to
  HAM8. The effects here are parametric, so the same arithmetic runs at whatever
  size the window is. Scale 1.0 stays exact and is the reference every other
  scale is checked against.
- **A gallery past the end.** The demo stops on the ark. `part_credits.c` then
  hands over to the demo's own things with controls on them - the vessel, the
  cracked orb, the ark, the burning city, and the temple valley to fly through.
  There is no such part in the demo and it says so.

## Getting it running

You need the original release. It is **not** in this repository - see
[Legal](#legal). Put the two files here:

```
reverse/ghidra/relic        1,851,760 bytes
reverse/ghidra/relic.dat    4,068,002 bytes
```

Then pick a host. The core renders into a frame and does nothing else - no
window calls, no input, no file paths - so each host is a single file.

| Target | Build | Needs |
|---|---|---|
| A window | `src/relic.sln` in MSVC | SDL2 |
| PPM files | `src/relic_ppm.vcxproj` | nothing but the CRT |
| The browser | `sh tools/build_web.sh` | emscripten |

Both hosts default to the paths above, so from the repository root the
window needs no arguments at all:

```sh
relic.exe                       # or: relic.exe -w 1920 -h 1080
relic_ppm.exe reverse/ghidra/relic out/ -p 4 -t 971 -s 8
```

The web build is also the **only real C89 check** - emcc is clang at
`-std=c89 -pedantic`, which is far stricter than MSVC. Run it even when you are
not shipping a web build.

### Keys

| Key | |
|---|---|
| `[` `]` | jump a scene back or on |
| `space` | pause, `.` steps one frame |
| `s` | fast forward, while held |
| `m` / `+` `-` | mute, volume |
| `r` | render divisor, 1 to 4 - cheaper and softer |
| `drag` | swing the view, in the scenes that take one |
| `c` / middle-click | look hold: a drag stays put, and drag after drag turns a full circle |
| right-click | put the view back to rest |
| `f` | fullscreen, web build only - the same pixels scaled up; `esc` returns |

In the gallery past the end: `tab` for the next thing to look at, drag to orbit,
wheel to dolly, right-click for rest, `z` and `x` per thing.

## Reading it

Start with **`docs/overview.txt`** - the demo's own facts, the hunk map,
and what is open. Then:

```
docs/effects/        one file per visual effect - the heart of it
docs/subsystems/     the renderer, display, timing, music
docs/formats/        the load file, the asset archive, the geometry
docs/reconstruction.txt   what this port does differently, and why
```

Documents are RFC-style plain text. Every claim carries its evidence:
`[verified]` from the code, `[observed]` from the running demo on hardware,
`[inferred]`, `[guess]`. An honest gap is recorded as an open question rather
than filled in.

The code follows the same rule. One C function per function in the
disassembly, carrying the name the database uses, and **every function says
where it came from** - `ORIGIN` and an address if it is the demo's, `OURS` if it
is not. That distinction is load-bearing: without it you cannot tell whether a
piece of arithmetic is the demo's or an invention.

```
src/            the reconstruction - C89, no host dependencies
src/main_sdl.c  the only file that loops
tools/          the archaeology - format crackers, map exporters, checks
reverse/exports/    names, types and signatures, exported from Ghidra
```

## Legal

A personal interoperability and preservation effort on a 1998 demoscene release.
**The original executable and data file are not redistributed** - you supply
your own copy. The notes under `docs/` and the reconstructed source under `src/`
are our own work product.

`src/third_party/` is not ours: ibxm (module replay) and prowizard (the P61
depacker) carry their own authors and terms.
