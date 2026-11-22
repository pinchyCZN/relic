#!/bin/sh
# RELIC - the emscripten build.
#
#     sh tools/build_web.sh [outdir]
#
# THERE IS NO CMAKE IN THIS PROJECT. MSVC builds Windows from
# src/relic.sln and this builds the web; neither generates the
# other, and this one GLOBS src/*.c so adding a part only means
# adding it to the .vcxproj.
#
# emcc is clang with -std=c89 -pedantic, which is far stricter than
# MSVC. Run this even when you are not shipping a web build: it is
# the only thing that actually holds the C89 line.
#
# LANDMINE: THE DEFAULT 64 KB STACK IS THE WHOLE BUDGET HERE, and
# nothing warns when a frame does not fit. p61a.c's depacker had a
# 128 KB array as a local, which ran off the bottom of the stack
# during Music_Preload and into the globals below it: the demo went
# on running perfectly with SDL's statics zeroed, so every window
# call answered "Video subsystem has not been initialized" and the
# canvas stayed black. A native build never sees it - Windows gives
# the main thread a megabyte. That array is static now; the cure for
# the next one is the same, NOT a bigger -sSTACK_SIZE.
#
# The demo's own files are preloaded into the virtual filesystem at
# the same relative paths the native build uses, so nothing in src/
# needs to know which host it is under.

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/web}

# THE WINDOWS EMSDK SHIPS emcc.bat AND NO EXTENSIONLESS SCRIPT, and a
# POSIX shell will not resolve one from the other. Worse, the .bat is
# not marked executable, so command -v never reports it however good
# PATH is - bash runs it anyway through its Windows layer, sh honours
# the mode bits and refuses. Calling it by its full path works in
# both, so the last resort is to go and look. EMCC=/path overrides.
if [ -z "$EMCC" ]; then
    if command -v emcc >/dev/null 2>&1; then
        EMCC=emcc
    elif command -v emcc.bat >/dev/null 2>&1; then
        EMCC=emcc.bat
    else
        OLDIFS=$IFS
        IFS=:
        for d in $PATH; do
            if [ -f "$d/emcc.bat" ]; then EMCC="$d/emcc.bat"; break; fi
            if [ -f "$d/emcc" ]; then EMCC="$d/emcc"; break; fi
        done
        IFS=$OLDIFS
    fi
fi
if [ -z "$EMCC" ]; then
    echo "emcc not found - install emsdk and run its env script" >&2
    echo "  https://emscripten.org/docs/getting_started/downloads.html" >&2
    exit 1
fi
echo "emcc: $EMCC"

mkdir -p "$OUT"
cd "$ROOT"

# THE OTHER HOSTS MUST NOT BE LINKED IN, and music_silent.c is one of
# them: it defines the whole Music_ API that music_ibxm.c does, so a
# glob that takes both fails at the link with ten duplicate symbols
# and nothing wrong in any file. relic.vcxproj takes music_ibxm.c
# alone for the same reason.
# AND THE GLOB IS NOT RECURSIVE, so the replayer and the depacker
# under src/third_party have to be named: relic.vcxproj lists them
# the same way.
SRCS=$(ls src/*.c | grep -vE 'main_ppm\.c$|agent\.c$|music_silent\.c$')
SRCS="$SRCS src/third_party/ibxm/ibxm.c src/third_party/prowizard/p61a.c"

echo "sources:"
for s in $SRCS; do echo "    $s"; done

"$EMCC" $SRCS \
    -std=c89 -pedantic -Wall -Wextra \
    -O2 \
    -I src \
    -s USE_SDL=2 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXIT_RUNTIME=0 \
    -s EXPORTED_RUNTIME_METHODS=callMain \
    --preload-file reverse/ghidra/relic@reverse/ghidra/relic \
    --preload-file reverse/ghidra/relic.dat@reverse/ghidra/relic.dat \
    --shell-file tools/web_shell.html \
    -o "$OUT/relic.html"

# THE PAGE IS TRACKED AND THE BUILD IS NOT, so it is copied rather
# than generated. relic.html is emcc's own shell; index.html is the
# page, and it holds main back until the click so the picture cannot
# start before the browser allows the sound.
#
# @BUILD@ becomes a stamp the page prints and hangs on every URL it
# fetches, so a cached wasm cannot pass for a fresh one.
BUILD=$(date +%Y%m%d-%H%M%S)
sed "s/@BUILD@/$BUILD/g" tools/web/index.html > "$OUT/index.html"

echo
echo "built $OUT/relic.html"
echo "      $OUT/index.html  - the page"
echo "serve it - a file:// page cannot read the preloaded data:"
echo "    cd $OUT && py -3.10 -m http.server 8000"
