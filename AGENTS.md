# AGENTS.md

Contributor guide for inkwell. [`README.md`](README.md) says what the project is and why; this
says how to work in it.

## The one-paragraph version

inkwell is the systems layer under [inkcell](https://github.com/mcereal/inkcell). C17, Linux
only, no threads, one epoll loop. Four areas: `base/` (the leaves), `runtime/` (the loop, the
signals, the crash report), `codec/` (bytes in, bytes out), `net/` (one hostname, one socket).
Arrows point down and `scripts/check-layers.py` holds them there. `make test` before every
push.

## Layout

```
include/inkwell/<area>/   the public surface of an area, flat
src/<area>/               the sources; a header and its source always share a filename
third_party/              somebody else's code, and our configuration of it
tests/framework/          the self-registering case runner
tests/suites/<area>_*.c   one file per subject
tests/support/            fixtures a second suite needed
tests/data/               captured bytes a codec is tested against
scripts/check-layers.py   the layering rule the compiler cannot see
scripts/check-vendor.py   the digest rule a vendored file is held to
```

`include/inkwell/<area>/` is flat and is the interface. How a source is filed under `src/<area>/`
is not part of it: moving a file between groups inside an area is not an API change. Find a
header's source by *filename*, never by path.

## Style

- clang-format 18, config in `.clang-format`. `make format` before pushing; CI checks it.
- `inkwell_` on everything public, `INKWELL_` on macros and enum members. A symbol carries the
  prefix of whoever owns it — that is the whole point of the split, so a name that came from
  somewhere else gets renamed on the way in rather than keeping its old spelling.
- Every public header is `#pragma once` or a guard, wrapped in `extern "C"`, and says in prose
  *why* it works the way it does. The reasoning is the valuable part; a signature can be read
  off the line below it.
- Functions return `0` or a negative `errno`. A refusal that a caller must distinguish gets its
  own enum rather than being folded into a generic failure.

## Rules that compile fine when broken

- **`base/` includes nothing of inkwell's.** It is the floor.
- **No threads.** Anything that would block gets a descriptor and a callback instead.
- **No application vocabulary.** Not in code, not in comments. Say "a peer", "a protocol", "a
  transport". The day a comment in here says "the radio" is the day this stopped being a
  platform layer.
- **A new area under `src/` needs an entry in `ALLOWED`** in `scripts/check-layers.py`. Adding
  one is a decision about the shape of the stack.
- **`#include <inkcell/...>` and `#include <mesh/...>` are build failures.** inkwell is the
  bottom; there is nothing below it but Linux. The layer check fails on both spellings.
- **A vendored file is upstream's.** `third_party/` is excluded from `make format` and its
  digest is checked against its own README by `scripts/check-vendor.py`, which `make test` runs.
  A fix goes upstream and comes back as a new revision; an edit in place is invisible in review
  and fails the check. The exclusion is not tidiness - clang-format over 3.6 MB of generated C
  is both an unreadable diff and a digest mismatch, and it happened once.

## Extracting something from mesh-client

Most of what lands here arrives the same way, and the order matters:

1. **Check what it actually depends on.** `grep -h '#include "' <file>` is the whole test. A
   candidate that includes an application header is not ready; find the seam first.
2. **Move the tests with it.** A component that arrives without the cases that held it is a
   downgrade, however clean the diff looks. If the cases live inside a larger suite, slice out
   the ones that belong to the component and leave the rest behind.
3. **Rename on the way in.** `mesh_*` becomes `inkwell_*`, and a name that was shaped by the
   application gets the better name now rather than later: `mesh_event_loop` is
   `inkwell_loop`.
4. **Generalise the prose, do not delete it.** The comments explain real decisions and are worth
   more than the code. Rewrite "the radio" as "a peer"; keep the war story about why a rearming
   timer used to starve the loop, because the next person will hit it too.
5. **Leave the seam in the application.** Where the component had an application-specific
   convenience — a lookup by one well-known UUID, say — the general form comes here and the
   two-line wrapper stays there.

## Tests

```bash
make test                                  # the whole verify step
./build/tests/inkwell_tests --list
./build/tests/inkwell_tests --filter loop  # substring match
./build/tests/inkwell_tests --suite codec_zip
```

Cases register themselves from constructors, so adding one is a single `INKWELL_TEST_CASE` in a
single file. A new *suite file* goes in `INKWELL_TEST_SUITES` in `tests/CMakeLists.txt` — the
suite files are compiled straight into the executable rather than into a library, because a
linker is free to drop a library member nothing references and would take every case inside it
with it, silently.

A helper used by one suite stays `static` in it and moves to `tests/support/` when a second
suite needs it.

## Pull requests

- `make test` green, `make format` clean.
- Conventional Commits (`feat:`, `fix:`, `refactor:`, `docs:`, `test:`, `chore:`).
- An extraction PR says what it moved, from where, and what stayed behind — the last of those is
  the part a reviewer cannot get from the diff.
