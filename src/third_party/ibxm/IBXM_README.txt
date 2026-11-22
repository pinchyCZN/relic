ibxm/ac - MOD, S3M and XM replay by Martin Cameron.

    https://github.com/martincameron/micromod
    the ibxm-ac/ subdirectory, taken verbatim

Only ibxm.c and ibxm.h are used; the repository's xm2wav.c,
sdlplayer.c, patterndisplay.c and topaz8.c are example programs and
are not copied.

BSD 3-clause - LICENCE.txt. Not ours to commit, so .gitignore
excludes src/third_party/ the same way it excludes SDL.

IT IS ALREADY STRICT C89. Its own Makefile builds with
-ansi -pedantic -Wall, so unlike most vendored code it survives
tools/build_web.sh's C89 gate without being compiled separately.
