# AGENTS.md

Contributor guide for inkwell. [`README.md`](README.md) says what the project is and why; this
says how to work in it.

## The one-paragraph version

inkwell is a single-threaded C runtime for small native programs that talk to devices and
networks; [inkcell](https://github.com/mcereal/inkcell) is the UI toolkit that stands on it, and a
headless program uses it alone. C17, no threads, one loop, three target systems - epoll on Linux,
kqueue on macOS, waitable handles on Windows. All three ship; none is a development host. Six
areas: `base/` (the leaves), `runtime/` (the loop, the signals, the crash report), `codec/`
(bytes in, bytes out), `net/` (one hostname, one socket, one TLS session, one request), `ble/`
(one Bluetooth LE central), `io/` (the serial ports the system has). Arrows point down and
`scripts/check-layers.py` holds them there. `make test` before every push.

Two optional dependencies, both optional by presence. Mbed TLS is a submodule; without it
`net/tls.h` refuses every session. libdbus-1 is a system package on Linux; without it
`ble/central.h` refuses every call with -ENOSYS. Either way everything else builds and tests
exactly as it does with it.

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
  own enum rather than being folded into a generic failure - `net/reason.h` is the worked
  example, and the one thing it is strict about is that a reason never carries a word.

  That is a rule about *status*. A function whose answer is a quantity returns the quantity, and
  a negative errno when it has none - `inkwell_base64_encode()`, `inkwell_log_file_compact()`,
  `inkwell_text_utf8_next()` and `inkwell_stream_pump()` all do. The test is whether a caller wants
  the number: if it does, hiding it behind an out-parameter to satisfy the shape of the rule
  makes every call site longer and none of them clearer.

## Rules that compile fine when broken

- **`base/` includes nothing of inkwell's.** It is the floor.
- **No threads.** Anything that would block gets a descriptor and a callback instead. The one
  exception is not ours: CoreBluetooth delivers on a dispatch queue, and `src/ble/corebluetooth.m`
  keeps that thread to copies and a wake - nothing it touches is shared with the loop.
- **Only `runtime/` and `base/fd.c` say which kernel this is.** Everything else registers
  `INKWELL_LOOP_IN`/`_OUT`, never `EPOLLIN`, and asks `runtime/timer.h`, `runtime/wake.h` and
  `base/fd.h` for a timer, a wake, a pipe or a socket rather than calling `timerfd_create()`,
  `eventfd()`, `pipe2()` or `SOCK_NONBLOCK` itself. Those compile on Linux and nowhere else, and
  the macOS CI job is the thing that notices.
- **A platform gap refuses; it does not disappear.** Where a system has no backend yet, the area
  compiles a `<name>_unavailable.c` beside `<name>.c` and picks it in `CMakeLists.txt`: the header
  is unchanged, every symbol links, every call reports itself unavailable. An `#ifdef _WIN32`
  around a public declaration is the wrong fix - it moves the platform question into every
  caller. Closing a gap is swapping that file for a real backend and updating the platform table
  in `README.md`.
- **No application vocabulary.** Not in code, not in comments. Say "a peer", "a protocol", "a
  transport". The day a comment in here says "the radio" is the day this stopped being a
  platform layer.
- **A new area under `src/` needs an entry in `ALLOWED`** in `scripts/check-layers.py`. Adding
  one is a decision about the shape of the stack.
- **`#include <inkcell/...>` and `#include <mesh/...>` are build failures.** inkwell is the
  bottom; there is nothing below it but the operating system. The layer check fails on both spellings.
- **A vendored file is upstream's.** `third_party/` is excluded from `make format` and its
  digest is checked against its own README by `scripts/check-vendor.py`, which `make test` runs.
  A fix goes upstream and comes back as a new revision; an edit in place is invisible in review
  and fails the check. The exclusion is not tidiness - clang-format over 3.6 MB of generated C
  is both an unreadable diff and a digest mismatch, and it happened once. The same applies to
  `third_party/mbedtls`, where the pinned SHA is what a digest is for a vendored file; the
  configuration of it next door in `third_party/mbedtls-config/` is ours and is edited freely.
- **A dependency may not be compulsory.** `net/tls.c` compiles to a refusing stub without the
  Mbed TLS submodule, and `ble/none.c` stands in for BlueZ without libdbus-1, so a plain
  `git clone` on a bare machine is still a complete, buildable, testable inkwell. CI has a job
  that builds that way, because the claim is worth nothing if nobody checks it. A third
  dependency, if it ever arrives, arrives the same way.

## Extracting something from mesh-client

[`docs/extraction.md`](docs/extraction.md) is the running map: what has already come down, what
is next, and the evidence for each candidate. The error-vocabulary question that used to block
several rows is answered - `net/reason.h` - and where inkcell ends is the one still open. Read
it before picking something up.

Most of what lands here arrives the same way, and the order matters:

1. **Check what it actually depends on.** `grep -h '#include "' <file>` over the source **and
   its header**, then check what the source *calls* across the line. A candidate that includes
   an application header is not ready; find the seam first. Reading only the source is how
   `stream_link.c` sat in `docs/extraction.md` as a file move for a tranche - it includes one
   application header, its own, which includes two more.
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
