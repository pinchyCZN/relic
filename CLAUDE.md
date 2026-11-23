# PCDEMOS/relic - Amiga AGA demo reverse engineering

**`README.md` is what this project is and how to build it. This file is
the rules for working in it**, and it is the one document here exempt
from rule 10 - see 10.2.

## Project context

Reverse engineering RELIC by Nerve Axis - the winner of the Amiga demo
competition at Assembly 1998, Helsinki, 7-9 August - in Ghidra: naming
it, typing it, and reconstructing its visual effects.

**The mission is the EFFECTS, not the whole program.** The loader, the
memory pools, the `relic.dat` reader, the ReadArgs parsing and the
Player 6.1 music replay are means to an end. Attribute them, bound
them, and leave them. What we are after is the inner loop of each
visual effect and the maths behind it.

**One demo, one program.** There is no corpus here - everything in
`docs/` is about RELIC, so nothing needs a per-demo directory and no
fact needs qualifying with which demo it belongs to.

```
reverse/ghidra/relic          1,851,760 bytes   1998-08-11   the executable
reverse/ghidra/relic.dat      4,068,002 bytes   1998-08-11   the asset archive
reverse/ghidra/nvxrelic.readme   12,110 bytes   1998-08-11   the Aminet readme
```

`docs/overview.txt` holds the demo's own facts - the hunk map, the
subsystems the strings name, what `relic.dat` contains and what is
open. **Read it before any effect note.**

### This is 68020 big-endian Amiga, and that shapes everything

- **Big-endian, flat 32-bit addresses.** No segment:offset pairs, no
  segment arithmetic, one spelling per address. Simpler than the 64k
  project's real-mode world in exactly this one respect.
- **`68000:BE:32:MC68020`, NOT the loader's default.** The amiga loader
  hardcodes `68000:BE:32:default` in `findSupportedLoadSpecs`
  [verified, AmigaHunkLoader.java:98], and the demo requires 68020+ -
  it says so on its own requirements screen [verified, seg0+0x0183b7].
  **Override the language in the import dialog.** A 68000 disassembler
  does not reject 68020 instructions, it mis-decodes them: `bfextu`,
  `muls.l`/`divs.l`, scaled-index addressing and long bitfields all
  come out as something else, silently. This is the most expensive
  mistake available at import time.
- **We get real symbols, unlike the 64k project.** Three sources, all
  free:
  - the `ghidra-amiga` analyzer resolves library-base-relative calls -
    `jsr -552(a6)` becomes `OpenLibrary` - from the `.fd` files in the
    extension's `data/fd/`.
  - `amiga_ndk39.gdt` gives genuine Amiga OS types: `BitMap`,
    `ViewPort`, `Interrupt`, `Custom`, and several hundred more.
  - **6,605 RELOC32 entries** [verified, see `docs/formats/hunk.txt`].
    Every reloc offset is a pointer by definition, which types a great
    deal of data for free. The 64k project has no relocations at all.
- **The demo is NOT crunched.** 7 hunks, two of them BSS, 6,605
  relocations, plaintext library names and 700-odd readable strings in
  hunk 0 [verified]. A cruncher is one tiny code hunk plus an opaque
  data hunk; this is a normally linked program. **So there is no
  packed/unpacked split and no depacking stage** - the 64k project's
  rule 3 has no counterpart here.
- **It is HAND-WRITTEN 68020 assembly - 33,495 lines of it**, which the
  authors say themselves [verified, `nvxrelic.readme`]. So there is no
  compiler runtime to fence off, no library code to attribute, and no
  calling convention the decompiler can lean on. Every routine is
  somebody's hand-written one.
- **Custom chip hardware is the point.** Copper lists, the blitter,
  Paula audio DMA and AGA's HAM8 mode are what the effects are built
  out of. `0xDFF000` is the custom register block and the extension
  maps it for us. An effect here is often *a copper list plus a blitter
  job*, not a pixel loop.

---

## Hard rules

### 1. Git is read-only

Enforced in `.claude/settings.json`. Read commands (`status`, `log`,
`diff`, `show`, `blame`, `ls-files`, `grep`, ...) are allowed.
Everything that mutates a repo, an index, a working tree, or a remote
is denied - including `add`, `commit`, `push`, `pull`, `fetch`,
`checkout`, `switch`, `restore`, `reset`, `rebase`, `merge`, `stash`,
`clean`, `tag`, `remote`, `config`, `init`.

**Commits, branches, and pushes are the user's job, never the agent's.**
When a change is ready to commit, say so and stop.

> This is a guardrail against accident, not a security sandbox. Do not
> route around it - no `sh -c`, no aliases, no helper script that shells
> out to git. If a git write is genuinely needed, ask for it.

**THIS DIRECTORY IS NOT A GIT REPOSITORY.** `git init` is denied, so
the agent cannot create one; ask. Until it exists there is no history
of any kind, so **every working-tree file is the only copy** - there is
no restoring content from a commit, because there are no commits. To
undo something, make targeted edits to the working copy.

### 2. Ghidra is the source of truth for the binary

Never guess at an address, offset, field, or signature. Read it out of
the database, or out of the generated maps that come from it (rule 6).
If the database is unreachable, stop and say so - do not substitute
recollection for a lookup.

The database is live and shared with the user's open Ghidra GUI.
Renames, retypes and comments are real edits to their project. Batch
them deliberately; do not churn.

```
project   relic        reverse/ghidra/relic in this repository
program   relic
server    discovered by project name - see 2.1, NOT a fixed port
```

#### 2.1. THE PORT TRAP - check it before every session's first write

**A PORT NUMBER DOES NOT IDENTIFY A GHIDRA PROJECT.** More than one can
be open on a machine, and GhidraMCP takes the first free port in its
range, so the instance that launched first gets the lowest and the
others follow. **relic has been observed on 8089** - which is also the
port a user-scope config may pin - but launch order can put a different
project there on any given day.

A read against the wrong database is a wasted call. **A WRITE AGAINST
THE WRONG DATABASE SILENTLY CORRUPTS SOMEBODY ELSE'S WORK**, and it is
the one mistake here that cannot be fixed by looking something up
again.

So, as the first MCP call of any session:

```
mcp__ghidra__list_instances
```

and read the **`project` field, never the port**. IT MUST SAY `relic`.
Any other name means the MCP is pointed somewhere else and is unusable
here until that is fixed - do not work around it with
`connect_instance`, which the bridge can report success for while
connecting elsewhere.

`tools/gq.py` applies the same rule mechanically: it scans ports
8089-8099, picks the instance reporting project `relic`, and refuses to
run against anything else.

```
py -3.10 tools/gq.py who                      prove which database
py -3.10 tools/gq.py find   list_             search the tool names
py -3.10 tools/gq.py sig    rename_data       what a tool takes
py -3.10 tools/gq.py call   get_metadata program=relic
```

`DEMO_GHIDRA_URL` overrides the scan when the port is already known.

**Do not pin a port anywhere in this project.** `.mcp.json.example`
deliberately sets no `GHIDRA_MCP_URL`, because a pin is correct only
until the next time two Ghidras start in a different order, and a stale
pin fails by connecting to the wrong database rather than by erroring.
`.mcp.json` itself is gitignored: it names one machine's layout.

#### 2.2. Strict program routing is on - name the program every time

`GHIDRA_MCP_REQUIRE_PROGRAM_SELECTORS=all`. Every call must carry
`program="relic"`. A call that omits it is refused rather than silently
answered from whatever the server had open.

Only one program is open here, so the refusal protects little. Obey it
anyway - it is one word, and it is what keeps a command copied in from
somewhere else from doing something surprising.

### 3. The import: get it right once

A mis-imported program poisons every address in the project, and the
cost of noticing late is every document written in between.

**The import must be:**

```
loader     Amiga Hunk Loader (ghidra-amiga)
file       reverse/ghidra/relic
language   68000:BE:32:MC68020      <- CHANGE IT. The loader offers 68000
image base 0x21F000                  the loader's DEF_IMAGE_BASE [verified]
```

**The expected result is in `docs/overview.txt` section 3** - seven
segments, `SEG0` at `0x0021f000`, the image ending at `0x003e43cf`, a
`start` function at the image base. That map is computed from the
loader's own source, not yet observed [inferred]. **Check the real
import against it and correct the document from the database**, which
is the first task of the session that imports.

#### 3.1. The loader was broken and is now patched

`ghidra-amiga` could not read this file at all. The import died with

```
hunk.HunkParseError: HUNK_HEADER contains invalid hunk_size
```

because `HunkHeaderBlock.java:51` read each allocation size into a
**signed** Java `int` and rejected it when negative - and bits 30-31 of
that longword are the `MEMF_` memory flags, so every `MEMF_FAST` hunk
reads back negative. Five of RELIC's seven hunks are `MEMF_FAST`. The
full diagnosis is `docs/formats/hunk.txt` section 3.

```
py -3.10 tools/ghidra/fix_amiga_loader.py --check     is it patched?
py -3.10 tools/ghidra/fix_amiga_loader.py             patch it
py -3.10 tools/ghidra/fix_amiga_loader.py --restore   put .orig back
```

The jar in the user's Ghidra install **is patched**, with the pristine
copy kept beside it as `ghidra-amiga.jar.orig`. Two things follow:

- **A Ghidra or extension reinstall silently reverts it** and the import
  starts failing again. Run `--check` before blaming the file.
- **`reverse/patched/relic_anymem` is the fallback**, not the working
  image: the same bytes with the seven flag longwords cleared, which a
  stock extension accepts. It is NOT kept in the tree - `reverse/patched/`
  is gitignored because the file is the original with seven bytes
  changed. `tools/hunk/hunk_memf_any.py` regenerates it on demand, so
  the project is not hostage to a modified install.

> **The two images have IDENTICAL addresses.** The loader ignores
> memory flags entirely - `Relocate.getSeqAddresses` lays segments out
> sequentially from the image base whatever they say - so unlike the
> 64k project's packed/unpacked pair, an address needs no qualifying
> here. Seven bytes of header differ and nothing else does. Do not
> build a "which image" discipline this project does not need.

### 4. Write down what you discover

A finding that exists only in a chat transcript is lost. Anything
learned about a struct, an effect, a subsystem, or a file format goes
into `docs/` **in the same session it was found**. See `docs/README.txt`.

### 5. Separate fact from inference

Every claim in `docs/` carries a tag: `[verified]`, `[inferred]`,
`[observed]`, `[guess]`. `docs/README.txt` section 2 defines them and
says why `[observed]` outranks `[inferred]` for anything visual.

**Renaming a Ghidra symbol is an assertion. Do not assert what you have
not earned.** A name you are not sure of carries `maybe_` and a plate
comment saying what is unconfirmed.

**A type carries its tag in its Ghidra description**, whose first line
is fixed:

```
[verified] origin=0021f000  the AmigaDOS entry point
```

The exporter reads that line, so the tag and the origin address reach
`struct_map.txt` without a second copy anywhere. A type with no tag
shows up as `src=untagged` and `structmap.py check` reports it. That
report is the most common way a session ends with the work done and the
provenance lost - clear it before stopping.

### 6. Ghidra is a deliverable, not a reference

**This is the central rule of the project.** Low-level research that
turns up something the database does not have goes back INTO the
database, in the session that found it. A name read off a string, a
signature proved by a disassembly, a field whose width you just
established from a `move.l` - all of it lands in Ghidra. Leaving it in
a chat reply means the next session rediscovers it, and the database
stays the weakest copy of what we know.

So every analysis task closes with three things done, not one:

| Done | Where |
|---|---|
| **renamed** | the function, its parameters, its locals, the globals it touches |
| **retyped** | a struct applied at every site, not just the one being read |
| **documented** | a plate comment on the function; the note in `docs/` |

A function you read and understood and left as `FUN_0021f0a4` is work
thrown away. If the understanding is too thin to name it, say that in a
plate comment on it - that is still a deposit.

#### 6.1. Naming conventions

- **Functions:** `Subsystem_Verb` - `Dat_Open`, `Plasma_Render`,
  `Copper_Build`, `P61_Init`. One style, not two.
- **Effects get the effect's name, and `relic.dat` has already named
  most of them.** `Fplasma`, `Faztecterrain`, `Fshiptextures`,
  `Ftemplelog`, `Fcrust`, `Fearth`, `Fsprout`, `Fwadepic` are the
  demo's own words for its parts [verified, `docs/formats/relic_dat.txt`].
  **Use the demo's name over a descriptive one** - `Aztec_*` beats
  `Terrain_*`. Where there is no name, describe it: `Tunnel_Render`.
- **Unverified:** prefix `maybe_` and leave a plate comment stating what
  is unconfirmed. Dropping the prefix later is cheap; a confident wrong
  name costs a future session a wrong turn.
- **Globals:** `g_` prefix.
- **Structs, unions, enums: prefix `Relic_`.** Not because another
  program might collide - there is only one program here - but because
  `amiga_ndk39.gdt` loads several hundred *genuine* Amiga OS types into
  the same database. An unprefixed `Screen` or `BitMap` of ours is
  indistinguishable from Commodore's, and the difference matters: one
  is documented and one is our guess. Anything that IS a real Amiga
  type is used as-is from the NDK and never redefined.
- **Never overwrite an existing meaningful name without saying why**,
  and treat an analyzer-supplied library name (`OpenLibrary`,
  `AllocMem`) as meaningful - it came from the `.fd` files and is
  right.

**The Ghidra server warns against all of the above and it is wrong for
this project.** It wants `DatOpen` over `Dat_Open`, a verb from a fixed
list as the first word, and `snake_case` labels over `g_`. Those
warnings are advisory, the write succeeds, and the conventions above
are the ones we keep. Ignore the
warning; do not rename to satisfy it, and do not re-litigate this per
session.

#### 6.2. Amiga specifics that will cost a session otherwise

- **A library call is `jsr -offset(a6)` and `a6` is a library base.**
  Which library depends on what was loaded into `a6`, which can be
  several basic blocks earlier. The analyzer resolves many of these; when
  it has not, establish the base before naming the call, and never
  assume `exec.library` because `-552` looked familiar.
- **`0x00000004` is `AbsExecBase`.** `move.l 4,a6` at the entry point is
  how every Amiga program starts; it is not a magic address to document.
- **The copper list IS code, and Ghidra will not disassemble it.** A
  copper list is a data array of `(register, value)` and `WAIT` words
  that the chipset executes once per frame. It will sit in the listing
  as undefined data. Type it, do not disassemble it, and write the
  display it builds into `docs/subsystems/`.
- **Self-modifying code and run-time-built copper lists and blitter
  lists are normal in this corpus, not a curiosity.** The bytes Ghidra
  shows may not be the bytes that execute. Note it on the function and
  say what the built value is.
- **Chip RAM versus fast RAM is a real distinction in the code.** Only
  chip RAM is reachable by the blitter, the copper and Paula, so an
  allocation's pool tells you what the data is FOR. `relic.dat`'s `C`
  and `F` name prefixes encode it, and the two `MEMF_CHIP` hunks are
  the chip-resident ones. See `docs/overview.txt`.
- **Code and data are freely mixed and the image is one rwx blob.**
  There is no executable-section test that means anything - hunks 2, 5
  and 6 are `HUNK_CODE` and may well be mostly data. What distinguishes
  code from data here is a reachable control-flow path, nothing else.

#### 6.3. Check the maps AND the comments before naming anything

Grep `reverse/exports/` and `docs/` for an address before assigning a
name, and search the plate comments too. A global's identity is often
already written in the comment of a function nobody has read yet. That
has cost three renames of one symbol across three sessions - once in
the wrong direction - while the answer sat in a plate
comment the whole time.

Searching for an address is cheap and belongs *before* reasoning about
what something means, not after.

#### 6.4. Save the database when the work lands

**Edits are live but UNSAVED. Call `save_program` after any batch of
real changes** - a struct created or retyped, functions renamed, plate
comments written, a script applied. Not after every single call; after
the batch that constitutes the finding.

An unsaved database is one closed window away from losing a session's
work, and the loss is silent: Ghidra reopens looking exactly as it did
before, so there is nothing to notice. Nothing else in this project is a
copy of it - `reverse/exports/` is generated FROM the database, so an
export is not a backup.

**Save before re-exporting the maps**, so the text files can never
describe a state the database would not come back as.

The one caution: saving commits whatever the user's own GUI session has
in flight along with ours. That is normally what they want, but if a
batch turns out to be wrong, say so rather than saving over it.

### 7. Struct layout comes from the maps, never from arithmetic

`reverse/exports/relic/` holds three files generated from the database
by `tools/ghidra/demo_export_maps.java`:

| File | What it holds |
|---|---|
| `struct_map.txt` | every struct, union, enum and typedef we defined |
| `symbol_map.txt` | named data globals - address, type, count, size |
| `function_map.txt` | every function carrying a name somebody chose |

They exist because reading a decompiled function means answering "what
is at `+0x18` of this thing" and "what is `0x0033be20`" dozens of times,
and a flat text file answers both with one grep instead of one round
trip apiece.

```
py -3.10 tools/structmap/structmap.py -p relic whatis 0x0033be20
py -3.10 tools/structmap/structmap.py -p relic off  Relic_Effect.frame
py -3.10 tools/structmap/structmap.py -p relic at   Relic_Effect 0x18
py -3.10 tools/structmap/structmap.py -p relic sym  Dat_Open
py -3.10 tools/structmap/structmap.py -p relic check
```

- **Consult the maps first.** Grep by hex offset, field name, symbol or
  address.
- **Do NOT calculate offsets by hand** when the type is in the map.
  `off` and `at` print their working, so the arithmetic can be checked
  against the instruction in front of you.
- **Do not hand-edit the maps.** They are generated; the database is the
  source of truth and the next export discards the edit.
- **Re-export after any session that applied a type or a name**, and run
  `structmap.py check`. A map older than the database is the one way
  this system lies to you.

```
py -3.10 tools/export_maps.py                   every open program
py -3.10 tools/export_maps.py relic             just one
```

#### 7.1. THE PROGRAM SELECTOR DOES NOT REACH A GHIDRA SCRIPT

Rule 2.2 makes `program=` mandatory on every call, so the habit is to
add it and trust it. **`run_ghidra_script` accepts it and ignores it.**
A script's `currentProgram` is whatever the Ghidra *tool* has active,
which is normally whatever was imported or clicked last - so the
selector names one program while the export describes another.

It is silent. The export succeeds, prints a plausible count, and the
only clue is the `program:` line the script itself prints. Two map
sets have been written describing the same program this way.

So: `switch_program` first, then run the script, then check that printed
line against what you asked for. `tools/export_maps.py` does all three
and fails loudly when they disagree. Use it rather than calling
`run_ghidra_script` directly, and assume the same trap for any future
script under `tools/ghidra/`.

With one program open this trap is mostly dormant here. It wakes up the
moment a second program is imported - a dumped `relic.dat` chunk, a
memory snapshot - so keep using the wrapper.

#### 7.2. ONE ADDRESS OR OFFSET HAS EXACTLY ONE OWNING SCRIPT

Apply scripts under `tools/ghidra/` **EDIT, they never REBUILD.** Two
ways of getting this wrong have cost real work:

- **A script that constructs a fresh `StructureDataType` and adds it
  with `REPLACE_HANDLER` deletes every field the other scripts
  defined.** Re-running one such script silently destroyed 24 recovered
  fields. Look up the existing type and edit it; only construct when it
  does not exist.
- **Two scripts naming the same address fight.** Re-running an older one
  reverted a confirmed name back to its `maybe_` form. When a later
  script takes over an address or an offset, DELETE the entry from the
  earlier one and leave a comment saying who owns it now.

The symptom is silent: the script reports success and the count it
prints is its own, not the total. Print both - "14 from here, 38 named
in total" is what caught it.

**A missing struct is a stop, not a guess.** An honest `undefined2`
beats a confident wrong field. Map what the evidence supports, leave the
rest undefined, and say at the end of the task which types are missing.

### 8. Plain ASCII in everything we write

Every byte in a document, script, or comment we author must be `<= 0x7F`.
No em dashes, curly quotes, arrows, box-drawing, or ellipsis glyphs.
Write `->`, `-`, `...`, `x`, and `+`/`-`/`|` for diagrams.

```
py -3.10 tools/asciify.py --check    ASCII, width, table alignment
py -3.10 tools/asciify.py --fix      transliterate and rewrap
```

The demo's own strings are not covered by this - they are data, read out
of the binary, and get quoted as they are. Neither is anything under
`reverse/`. Note that `nvxrelic.readme` is a 1998 Amiga ASCII-art file
full of high-bit characters; quote from it sparingly and transliterate
when you do.

### 9. Documents are RFC-style plain text, not markdown

Documents are `.txt`, all under `docs/`: project-level documents at the
top, effect reports in `effects/`, subsystem reports in `subsystems/`,
struct notes in `structs/`, file formats in `formats/`.

**A cross-reference is relative to `docs/`** - `effects/plasma.txt`,
`subsystems/music.txt` - never to the file's own directory.
`tools/checkrefs.py` resolves all of them and is in the closing
checklist. This file is the one exception - Claude Code requires the
name `CLAUDE.md` and the harness renders it.

```
PCDEMOS RELIC                                              Analysis Note


                          THE RELIC.DAT ARCHIVE


Abstract

   Body text indented three spaces, wrapped at 72 columns.

Table of Contents

   1. First Section
      1.1. A Subsection

1.  First Section

   +--------------+-------+
   | Column       | Count |
   +==============+=======+
   | aligned with | 2542  |
   +--------------+-------+
```

What follows from that:

- **No markdown syntax in a `.txt` file.** No `#` headings, no
  `**bold**`, no backticks, no `*emphasis*`, no `[text](link)`, no
  `|`-delimited markdown tables. If it needs a renderer to look right,
  it is wrong.
- **Tables are drawn with `+---+` borders and space-aligned columns**,
  `+===+` under the header row. Every `|` row must be exactly as wide as
  the border above it.
- **Numbered sections** (`1.`, `1.1.`) with a Table of Contents.
- **Body indented three spaces, wrapped at 72 columns.** Tables may run
  to 120 including the indent, and so may a deeply indented listing.
- Blocks indented four or more spaces are preformatted - address maps,
  disassembly, diagrams - and are never rewrapped.
- Keep an **Open questions** section in every document. An honest gap is
  more useful than a confident guess.

`py -3.10 tools/rfcfmt.py --fix P` renders a markdown-ish draft as RFC
text. It is a one-way converter, not a round-trip formatter - it refuses
a file already in RFC form. Author new documents in RFC form directly,
and if you do use it, **read the output**: it strips inline markup, so a
passage about markdown comes out mangled.

The full conventions, the evidence tags and the templates are
`docs/README.txt`.

### 10. Record facts, not the journey - and record each one once

Documentation says **what the code does and why**. It does not narrate
the search that found it. Do not write "we first thought X, then found
Y" - write Y. No "this took three sessions", no restating what a
previous section established, no summary of the summary.

The one exception is a marked trap: if an approach looks obviously right
and is actually wrong, say so in a sentence. "The loader's default
language is 68000 and this demo is 68020" is a fact about the problem.
"I imported it as 68000 first" is a diary entry.

**One fact, one home:**

| What | Where it lives |
|---|---|
| addresses, instruction-level evidence | the Ghidra plate comment |
| struct offsets, strides, sizes | the Ghidra type -> `struct_map.txt` |
| a type's evidence tag and origin address | the Ghidra type description |
| what an effect does, and its maths | `docs/effects/` |
| what a subsystem does | `docs/subsystems/` |
| a file or asset format | `docs/formats/` |
| the demo's own facts, status, open list | `docs/overview.txt` |
| why the C is shaped this way; divergences | the `src/` comment |
| project rules | this file |

Everything else **points at** that home - `see effects/plasma.txt 3.2` -
and does not restate it.

**Correct in place; never append.** When a fact changes, edit the
sentence that is now wrong. Leaving it standing with a correction beside
it produces two statements, one of them false, and the reader cannot
tell which.

**AN ANSWERED OPEN QUESTION IS DELETED, not annotated.** Move its fact
into the body and remove the bullet. `ANSWERED:` and `RESOLVED:`
prefixes in an Open questions list are the most common way this rule
gets broken.

#### 10.1. The test, and the check

This rule is older than every document here and gets broken anyway, so
it has a tool rather than another paragraph.

```
py -3.10 tools/journalcheck.py          every hit, with its category
py -3.10 tools/journalcheck.py --count  just the totals
```

Every pattern in it was measured against the whole of `docs/` before
being added. Ones that fired on legitimate prose were dropped rather
than kept with exceptions - a check with known false positives stops
being read.

#### 10.2. The journey has a home, so it stops leaking

**`notes/` is the only place the search may be written down.** It is
gitignored but for its README, no check reads it, and it may be
deleted at any time.

It exists because the journey previously had nowhere to go. Rule 10
bans it from `docs/` and rule 18 bans it from the code, so it went
into whichever was being written - and once `commentbudget.py`
started biting on source, the leak moved into `docs/`. A note with a
home does not leak.

**A fact in `notes/` is a fact lost.** `checkrefs` does not resolve
its links and nobody greps it. The test is still rule 10's: the
BINARY goes to `docs/`, the history of our work on it goes here.

#### 10.3. Comments must sit against what they describe

```
py -3.10 tools/commentcheck.py             every finding
py -3.10 tools/commentcheck.py --selftest  prove each rule fires
```

`journalcheck.py` polices what a comment SAYS. This polices where it
SITS and whether it still points at anything:

| Finding | What it means |
|---|---|
| **orphaned** | a block comment with nothing under it - something got inserted between it and what it described |
| **stacked** | two block comments in a row - a second explanation bolted beside the first, not replacing it |
| **dead-origin** | an `ORIGIN` address no export carries any more |
| **bare-address** | an address in a comment that is in no export, so a reader cannot check the claim |

**An orphaned comment is the characteristic AI failure here** and it
accrues silently: each insertion is locally reasonable and the
wreckage only shows up when someone reads the file top to bottom.

The address checks are the stronger half. An `ORIGIN` is the only
part of a comment that survives a rename, so a stale one is worse
than no comment - and a bare address in no export means rule 6 was
skipped: the finding was written into a comment and never into the
database.

**Run `--selftest` after touching the tool.** It caught a divider
rule that matched any short comment and silently disabled the
`stacked` check; a linter that has never been seen to fire is not a
linter.

#### 10.4. Why there is no journal database

A store of comments keyed by file and function, with the code left
bare and a sweep at the end, has been considered and rejected. It
would make things worse in four ways:

- **Distance causes staleness; it does not cure it.** A comment
  beside the code is at least seen when the code changes. One in a
  database never is.
- **It breaks `ORIGIN`.** The address is the link back to the
  binary and has to be AT the code - rule 18.
- **It deletes the forcing function.** `commentbudget.py` works
  because comments cost lines in a file. A database is unbounded,
  which is the condition under which more gets written, not less.
- **Dangling rows are the orphan problem, moved somewhere nobody
  looks.**

The staging instinct behind it is sound, and `commentbudget.py
--reduce` is where it lives: a sweep that happens continuously
instead of one deferred to an end that never arrives.

**THE TEST A SENTENCE MUST PASS: does it describe the BINARY, or the
history of our work on the binary?** A reader who has never seen a
previous session should not be able to tell that there were any.

| Banned | Write instead |
|---|---|
| "this was called `Music_Isr` for several sessions" | "`P61_TimerInterrupt` is the replayer's CIA-B handler" |
| "ANSWERED: `Fcrust` is a texture" | delete the bullet; state it in the body |
| "the first string sweep missed them" | "hunk 0 holds 724 printable runs" |
| "that cost four wrong names" | nothing - delete it |

Both columns carry the same fact. The left also carries us.

**CLAUDE.md itself is exempt and nothing else is.** A rule needs its
rationale, and "this cost three renames of one symbol" is what stops
the next person undoing it. That licence does not extend to
`docs/`.

### 11. Running the demo

**The agent MAY run RELIC under an Amiga emulator**, and should want
to - this is a visual artifact and `[observed]` evidence is the only
kind that settles what an effect looks like.

It is a heavier proposition than running a DOS demo under an
emulator, and the requirements are the demo's own, off its
requirements screen
[verified]:

```
68020+ CPU          AGA chipset         PAL display
1700 Kb CHIP free   4000 Kb FAST free   relic.dat present
```

WinUAE or FS-UAE with an A1200/A4000 AGA configuration, a 68020 or
better, and Kickstart 3.x. The demo also wants `r:p61.relic2` - a file
it expects in `ram:` - and takes `SN=SNAPSHOT/S,HI=HIRES/S` as
AmigaDOS arguments [verified, seg0+0x018960]. It can save HAM8
snapshots to `ram:` when RMB is held [verified, seg0+0x0185a9], which is
the cheapest way to get an exact frame out of it.

What that permission does not extend to:

- **THE TWO ORIGINAL FILES ARE STOCK AND STAY STOCK.** `relic` and
  `relic.dat` are the only copies and there is no git history to
  restore them from. Never write to `reverse/ghidra/relic*` - not to
  patch, not to "fix" the header. A tool that transforms either one
  writes to `reverse/patched/`, `reverse/extracted/` or `%TEMP%`, never
  over its input. `tools/hunk/hunk_memf_any.py` and
  `tools/dat/datlist.py` both refuse to.
- **An emulator run produces `[observed]` evidence, not `[verified]`.**
  A screenshot says what it looked like; it does not say what the code
  does.
- A prediction that would need a run to confirm stays tagged as a
  prediction until the run happens. Do not quietly promote it.
- Say what you ran and how, so the result can be reproduced.

### 12. Never fake the look

**Match the original's CODE, not a capture of it.** When a reconstructed
effect looks wrong next to the real demo, the fix is to find the code
that produces the right result - never to add something that produces
the right pixels.

Banned, however tempting: hardcoding a constant, colour or timing that
"looks right" instead of reading it out of the binary; inventing a
palette ramp to stand in for one we have not located; tuning a magic
number until the output matches a screenshot.

This rule carries particular weight here because **the assets are
sitting right there in `relic.dat`**, which makes the temptation
specific: it is easy to display `Fwadepic` and call the loader
understood, or to guess that a 0xC000-byte `Faztecterrain` is a 256x192
heightmap because that is a round number. The format is what the code
says it is. Read the reader.

`[observed]` evidence is for deciding what to go and read, and for
catching a wrong colour. It is never a licence to implement the
observation directly.

### 13. Python is `py -3.10`

Run scripts through the launcher: `py -3.10`, which finds the
interpreter wherever it is installed. Do not assume `python` is on
PATH. Ask before assuming a pip install; nothing beyond the standard
library is required by anything in `tools/` today.

### 14. Scratch files go to the temp directory

Test exports, one-off scripts, and debug output go to the session
scratch directory or `%TEMP%`, resolved via the environment variable -
never a folder literally named `%TEMP%` inside the repo. Repo paths are
for persistent artifacts: `reverse/exports/*.txt`,
`reverse/extracted/*`, `docs/*.txt`, `sim/`, `src/`.

`structmap.py` honours `DEMO_EXPORTS` so a trial export can be checked
in a scratch directory before it replaces the real one.

---

## The reconstruction

**`src/` IS THE RECONSTRUCTION. `sim/` IS THE ARCHAEOLOGY BENCH.**
They are split by the KIND of work, not by stage, and neither feeds
the other.

An effect is proved by LOOKING at it, and for a long time Python was
the fastest way to look. It is not any more, for two reasons. The
effects ahead are per-pixel - the logo's field alone is 40,962 cells
a frame - and CPython is two orders of magnitude short. And every
Python transcription carries `s32()`, `& 0xffff` and `sdiv()`
scaffolding to imitate a 68020, where C89 just has `int`, `short`
and `/`. The target language is a closer match to the source
language than the intermediate one was.

So EFFECTS GO STRAIGHT INTO `src/`. There is no Python prototype
stage.

What `sim/` and `tools/` keep doing is the part Python is still best
at: cracking formats. The wins this project has had - recovering a
bitmap's stride by scoring row correlation, finding an image's
aspect by trying every byte-aligned width, settling a bit sense from
ink-versus-word-length, locating a palette by noticing a 0x400 gap -
were all data archaeology, and none of it was rendering. That work
continues in Python and its results land in `src/`.

**This is a REIMAGINING, not an emulation.** An emulator already
reproduces RELIC exactly and there is no reason to write another.
The effects are resolution-parametric and the judgement calls are
the point - but the line is narrow: **enhance the sampling of the
demo's own data, never substitute data of our own.**
`docs/reconstruction.txt` section 2 draws it, and rule 12 still
bans inventing a colour, a ramp or a constant.

**TWO BUILD SYSTEMS, NEITHER GENERATING THE OTHER.** There is no
CMake. `src/relic.sln` is hand written and tracked;
`tools/build_web.sh` globs `src/*.c` for emscripten.

```
src/relic.sln                  open in MSVC - targets relic, relic_ppm
sh tools/build_web.sh          the web build; THE ONLY REAL C89 CHECK
py -3.10 tools/ctest.py        src/ vs sim/, byte for byte at scale 1
py -3.10 sim/run.py --list     the Python viewer, while it lasts
```

### 15. A transcription, not an impression

This applies to `src/` first and to whatever is left of `sim/`. Rule
12 applies hardest of all here, because a window is exactly where
faking it is easiest and most tempting.

- **One function per Ghidra function, carrying the database's name**,
  lowercased to Python taste - `Plasma_Render` becomes `plasma_render`.
- **Every routine's docstring opens with `ORIGIN` and the address**, so
  any number in the file can be checked against the listing.
- **Transcribe the form the code uses, not the form the maths reduces
  to.** If the code is a sum of two sine lookups that happens to be
  separably `2 sin(y) cos(2x)`, `sim/` writes the sum and `docs/`
  records the identity. The identity is a description, not a licence to
  substitute.
- **Keep the integer widths and the wrap.** A `& 0xffff`, an `asr #7` on
  a value stored as a byte, a `divs` that truncates toward zero where
  Python's `//` floors - all of it is load-bearing. 68k is big-endian,
  so a word read out of a byte array is `>H`, never `<H`.
- **Where a value is not known, refuse.** No placeholder palette that
  happens to look like the capture. Show the index as grey and say so in
  the window title. A plausible ramp would be a rule 12 violation
  wearing a proof-of-concept hat.
- **Anything ours is labelled ours.** A viewer, a camera sweep or a
  frame slider that is not in the demo says so.
- The window is stdlib tkinter and **the blit must be raw P6 PPM
  bytes**: `PhotoImage(data=b"P6
<w> <h>
255
" + rgb)`. No PIL.

  **The obvious tkinter route is a trap that costs the whole frame
  budget.** `PhotoImage.put()` with a string of `{#rrggbb ...}` tokens
  is what every tutorial shows, and it makes Tk parse ~3.4 MB of text
  per frame: **0.47 s against 0.001 s** for the same 760x560 pixels as
  PPM [measured]. It is not a micro-optimisation, it is the difference
  between 1 fps and 80.
- **Load and decode once, at startup.** `sim/run.py` reads every chunk,
  decodes every texture to ready-to-blit RGB rows, and parses the
  geometry in its `Assets` constructor. A redraw must touch no file.
  `py -3.10 sim/run.py --bench` times a frame without opening a window
  and is the check that this has not regressed.
- **Fill triangles by scanline, one slice assignment per row.** A
  per-pixel barycentric loop in Python costs about a microsecond a
  pixel - 205 ms a frame against 5 ms [measured]. `sim/` uses painter's
  ordering with no depth buffer for the same reason, and says so on
  screen.
- **AGA is not mode 13h.** A part may be HAM8, 256 colours, EHB, or a
  dual-playfield copper-split - the resolution and the colour model are
  per part, not per demo. Record both in the effect note, and do not
  assume a single framebuffer format across `sim/`.

### 20. The core knows nothing about a host

`src/` renders into a `Relic_Frame` and does nothing else - no window
calls, no input, no file paths outside `image.c`. A HOST picks the
frame up and puts it somewhere: `main_ppm.c` writes files and needs
nothing installed, `main_sdl.c` opens a window, and emscripten will
be a third.

**Shape every part as `frame(f, tick)`, never a loop.** emscripten
cannot run a `while (1)`; it needs `emscripten_set_main_loop`.
Writing it that way costs nothing now and is painful to retrofit.

**EFFECTS ARE RESOLUTION-PARAMETRIC, and the rule that keeps that
honest is: scale the pixel geometry, never the table domains.**
`g_BlobFalloff` is indexed `[radius << 8 | dist]` with radius masked
to 0x7f, so a radius-30 blob at 8x would run off the end of it at
240. Divide the pixel offset by the scale BEFORE the lookup instead
- that resamples the demo's own tables and keeps every index in the
range the demo uses.

Scale 1.0 stays exact and is the reference every other scale is
checked against. A part whose 1.0 output drifts is broken, whatever
it looks like zoomed.

### 16. ANSI C, and the target is readable C

**C89/ANSI C.** No `//` comments, no declarations after statements, no
`stdint.h`, no VLAs, no `inline`. Fixed-width types come from a project
header, so `u8`/`s16`/`u32` are ours and spelled once.

**`s64` IS ALLOWED, and is the one exception.** C89 has no 64-bit
type, but the 68020's `muls.l` makes a 64-bit product, so a
32x32 multiply that must not wrap is the original's width, not a
convenience. `relic.h` declares it with `__extension__` so clang's
`-pedantic` stays quiet. Do not hand-split a multiply into 16-bit
halves to avoid it - `FX_MUL` used to wrap at about 4,550 demo units
at 7.2x, and that overflow is what made a sprite write before the
frame buffer.

**TABS INDENT, SPACES ALIGN** in every `.c` and `.h`. One tab per
nesting level, then spaces out to the column a wrapped parameter list
or continued statement lines up under, so it holds at any tab width.

The goal is **C that reads like the effect**, not C that reads like a
decompiler. The decompiler's output is the starting point and no more:

- Reconstruct the maths. A reciprocal multiply is a divide and should be
  written as one, with the original's form noted if the rounding
  differs.
- Chip-register pokes become a port call or a named macro.
  `0xDFF096` becomes `custom_write(DMACON, ...)`. Say so once in a
  header comment; do not re-explain it per function.
- **A copper list is data that drives hardware; a C reconstruction has
  to decide what to do with it.** Either emulate the register writes
  per scanline or collapse the list into the display mode it produces.
  Whichever is chosen, say which in a header comment, because the two
  are not equivalent and a reader cannot tell from the code.
- Keep the original's integer widths and overflow behaviour where the
  effect depends on them, which for fixed-point effects is usually.
- Fixed-point stays fixed-point. Do not "clean it up" to float - that
  changes the output and makes a comparison against the original
  meaningless.

### 17. Import whole functions, not slices

**One C function per Ghidra function, carrying the database's name.**
Start from the decompiler's C and fix it up. Do not transcribe only the
part of a function the current task happens to need - a half-imported
function looks finished and is the hardest kind of gap to find later.

Where an effect is one giant asm routine with no call structure, split
it by what it does and say in a header comment that the split is ours.

### 18. Comments: `docs/` is the default, `src/` is the exception

**The failure mode this rule exists to stop: a one-line fix carrying
five lines of comment.** That is not documentation, it is a changelog
written into the code.

Comment **whole functions**, not individual edits. A function gets a
header comment saying what it does. A three-line fix inside it gets
**zero** comments unless it is one of these:

| Keep | Why |
|---|---|
| **`ORIGIN` + the address** | address first, name second - names drift, addresses do not |
| **`OURS`** | a routine with no counterpart in the demo |
| **a landmine** | looks obviously right, is wrong. State the PROBLEM as a fact, never the story |
| **a deliberate divergence** | so nobody "fixes" it back |
| **`[open]` / not implemented** | what is missing and what it would take |
| **a magic constant's provenance** | the address it was read from, because rule 12 depends on being able to check it |
| **a non-obvious WHY** | something reading the code cannot give you |

Everything else goes to `docs/` with a one-line pointer, or nowhere. In
particular, delete on sight: the dev journal ("we first thought X",
"fixed the crash by"); restatement of the code; our own debug toggles;
inline disassembly listings, since the `ORIGIN` address IS the listing;
and anything stale, which is the highest priority of all because a
stale comment actively misleads where a missing one merely omits.

**EVERY FUNCTION DEFINITION IN `src/` SAYS WHICH IT IS.** A
transcription opens with `ORIGIN` and the address it came from; a
routine of ours opens with `OURS`. There is no third case, and
`tools/commentcheck.py` fails the build on one.

The point is not tidiness. Without it a reader cannot tell whether a
piece of arithmetic is the demo's or an invention, and rule 12 turns
on exactly that distinction. `ORIGIN` goes on the DEFINITION, not
only on the declaration in a header - someone reading the `.c` needs
the provenance in front of them.

**`FUN_` and `DAT_` in a `src/` comment are a reference, not a stale
name.** Do not "fix" `FUN_0021f0a4` to whatever the database now calls
that address. The name carries the address, and the address is the only
thing that survives - lose the `.rep` and a friendly name points at
nothing while `FUN_0021f0a4` still points at the binary. Writing both is
fine; writing the friendly name *instead* is the loss.

The gate, because the criteria above are a judgement and judgement is
generous toward one's own fresh work:

```
py -3.10 tools/commentbudget.py            net comment <= net code
py -3.10 tools/commentbudget.py --reduce   a sweep: comment must DROP
py -3.10 tools/commentbudget.py --blocks   runs over 12 lines
```

Run the first before calling any change done. Going over it is the
signal to move the explanation to `docs/`, never to delete something
true. See `tools/README-commentbudget.txt` for what it is blind to.

> **The surrounding file is not the standard.** A dense neighbour reads
> as house style; it is evidence of the problem, not licence.

### 19. Discoveries flow BOTH ways

**The database and `sim/`/`src/` describe the same program. A fact
learned on one side is recorded on the other, in the same session.**

The failure this prevents is easy to walk into: work moves to the port,
a struct in `src/` turns out to be wrong, the agent goes back to Ghidra,
finds the missing field, fixes the C and not the database - and from
then on the two disagree with nothing saying which is right.

| Learned while... | Also required |
|---|---|
| fixing `sim/` against the binary | apply the field, name or type in Ghidra |
| reading Ghidra for a port question | write it into `docs/` AND the code |
| a struct gains or loses a member | update the Ghidra struct to match |
| a function's real name is settled | rename it in the database too |

This does not mean `sim/` must mirror the database's layouts. The
reconstruction may hold a different, simpler struct, as long as the
difference is deliberate and written down. What is banned is the two
drifting apart by accident.

---

## Repository layout

```
relic/
  README.md              what this is, and how to build it
  CLAUDE.md              this file - the rules, for an agent working here
  .mcp.json.example      the MCP servers; copy and fill in the paths
  .claude/settings.json  permissions (read-only git)
  notes/                 the journey - gitignored, rule 10.2
  docs/                  everything we discover - RFC-style plain ASCII .txt
    README.txt             doc conventions, evidence tags, templates
    overview.txt           THE DEMO'S facts, hunk map, status, open list
    effects/               one file per visual effect  <- the mission
    subsystems/            one file per subsystem
    structs/               one file per reconstructed type
    formats/               file and asset formats
      hunk.txt               the AmigaDOS load file, and the loader bug
      relic_dat.txt          the asset archive
  reverse/
    ghidra/                Ghidra project and the originals  [not committed]
      relic  relic.dat  nvxrelic.readme
      relic.gpr / relic.rep/
    patched/               derived images  [bytes not committed]
      relic_anymem           MEMF flags cleared; loads in a STOCK extension
    extracted/             relic.dat chunks  [bytes not committed]
    exports/               GENERATED from the database - see rule 7
      relic/
        struct_map.txt / symbol_map.txt / function_map.txt
  sim/                   the archaeology bench - rule 15
  tools/
    gq.py                  find and talk to the right Ghidra - rule 2.1
    export_maps.py         the maps - rule 7.1
    checkrefs.py           every doc cross-reference resolves
    journalcheck.py        no dev-journal writing in docs  [rule 10]
    commentcheck.py        comments anchored, addresses live [rule 10.3]
    ctest.py               src/ vs sim/, byte for byte at scale 1
    anchor.py              which TICK a captured frame sits at
    statecmp.py            that tick's state, demo vs ours
    shotgrid.py            a frame with a grid: RGB and LIGHT
    asciify.py             ASCII and width rule: --check / --fix
    tabify.py              tab indent, space alignment - .c/.h only
    rfcfmt.py              markdown draft -> RFC text
    commentbudget.py       no more comment than code  [rule 18]
    README-commentbudget.txt
    hunk/
      hunkdump.py            walk the load file, block by block
      imgaddr.py             image address <-> file offset
      hunk_memf_any.py       the MEMF-clearing fallback - rule 3.1
    dat/
      datlist.py             list and extract relic.dat
    ghidra/
      fix_amiga_loader.py    repair the extension's hunk_size bug - rule 3.1
      demo_export_maps.java  the database -> the three maps
    sprout/
      walkdecode.py          decode the light's octree walk routines
    structmap/
      structmap.py           query and validate the maps
  src/                   THE RECONSTRUCTION - C89, no host deps
    relic.h                types, the pixel layout, the frame
    image.h/.c             the executable addressed by image address
    dat.h/.c               relic.dat, which most parts read something from
    gfx.h/.c               the shared primitives - rule 20
    rast.h/.c              the triangle rasteriser
    parts.h/.c             the part registry
    space.h/.c             the space backdrop, shared by its scenes
    ship.h/.c  geom.h/.c   the vessel, and the geometry chunk behind it
    orb.h/.c  orbscene.h/.c  the cracked sphere and its scene
    objset.h               the temple module's meshes and raycast ground
    stills.h/.c            the valley and city module, shared by both
    sprout.h/.c            the light's octree
    beam.h/.c  tex.h/.c  uv.h/.c   arcs, textures, UV generation
    part_*.c               one file per part
    host.h                 the host boundary and the music clock
    demo.c                 the frame-at-a-time object
    music_ibxm.c           the replayer; music_silent.c is the mute clock
    agent.h/.c             the debug agent - subsystems/agent_mcp.txt
    main_sdl.c             host: a window
    main_ppm.c             host: PPM files, links only the CRT
    relic.sln              hand written, tracked - rule 20
    relic.vcxproj  relic_ppm.vcxproj
    third_party/ibxm/      module replay, not ours
    third_party/prowizard/ the P61 depacker, not ours
    third_party/SDL/       [bytes not committed]
```

## Working method

Anything run more than twice becomes a script in `tools/`, with a header
comment saying what it does and how to run it. Ghidra scripts go in
`tools/ghidra/` and are run as described in rule 7.

### Closing a session

```
py -3.10 tools/gq.py who                        rule 2.1 - the right DB
py -3.10 tools/asciify.py --check
py -3.10 tools/checkrefs.py                     doc refs still resolve
py -3.10 tools/journalcheck.py                  rule 10 - facts, not us
py -3.10 tools/commentcheck.py                  rule 10.3 - anchored, live
save_all_programs                               rule 6.4 - FIRST
py -3.10 tools/export_maps.py
py -3.10 tools/structmap/structmap.py -p relic check
py -3.10 tools/commentbudget.py                 once sim/ or src/ has code
```

The save comes before the export, so the maps can never describe a state
the database would not come back as.

Then say what was renamed, retyped and documented, and what is still
open. If `check` reports an untagged type, the session is not finished.

## Legal note

A personal interoperability and preservation effort on a 1998 demoscene
release. The original executable and data file are not redistributed.
Notes and any reconstructed source are our own work product.
