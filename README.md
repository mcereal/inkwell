# inkwell

The systems layer under [inkcell](https://github.com/mcereal/inkcell): the part of a handheld
Linux application that is not its UI and not its domain.

inkcell is the toolkit that draws a screen and reads a gamepad. inkwell is what that screen sits
on — one epoll loop, the signals that stop it, a clock, a log, and the codecs that turn bytes
from off the device into something a program can hold. Neither knows what a program is *for*.

```
   application          mesh-client, and whatever comes next
        |
     inkcell           theme, fonts, layout, widgets, framebuffer, input
        |
     inkwell           loop, signals, clock, log, env, files, codecs
        |
      Linux            epoll, timerfd, signalfd, evdev, /dev/fb0
```

The arrows only point down. inkwell knows nothing about inkcell and less about any application;
`scripts/check-layers.py` is what holds that, because the linker will not.

## Why it exists

Both of these were extracted from [mesh-client](https://github.com/mcereal/mesh-client), a
Meshtastic client for the TrimUI Brick, and the split was made twice for the same reason. The
first pass took out everything that was never about Meshtastic *and was about drawing*, and that
became inkcell. What is here is the rest of that sentence: everything that was never about
Meshtastic and was never about drawing either.

The giveaway was that mesh-client's event loop, its DNS resolver and its TLS client all
`#include "inkcell/utils/log.h"`. A clock and a log line are not UI. They were in the toolkit
because the toolkit happened to be extracted first, and the day the loop moved out of the
application was the day that stopped being tenable — a platform layer that has to depend on a
*widget library* to write a log line has its arrows the wrong way round.

So the foundation moved down here, and inkcell stands on it.

## What is in it

| Area | What it owns |
|---|---|
| `base/` | The floor: a monotonic clock and a wall clock you can distrust, a levelled log that bounds its own file, `$PREFIX_`-namespaced environment knobs, whole-file reads, UTF-8 that counts characters rather than bytes. Includes nothing, including from each other's area. |
| `runtime/` | One epoll loop with a bounded number of fd sources, no threads anywhere, and `SIGINT`/`SIGTERM`/`SIGHUP` delivered through a signalfd so a shutdown runs the ordinary path instead of the default kill action. |
| `codec/` | Bytes in, bytes out: base64 in both alphabets, SHA-256, a non-allocating JSON reader, an HTTP/1.1 request formatter and response parser that takes its input in whatever sized pieces the network hands it, and a zip central-directory walker that works on a window of a file rather than the whole thing. A codec parses; it does not know what the bytes are for. |
| `net/` | One hostname turned into an address by forking a child that is allowed to block, because `getaddrinfo()` has no non-blocking form and `getaddrinfo_a()` starts threads. Reported back through the loop, never from the call that started it. |

Everything here is C17, freestanding of any framework, and allocates as little as it can get
away with. There are no threads and there will not be any: the loop is the concurrency model.

## The rules it is built on

These are authoring rules — breaking one compiles and looks fine.

- **`base/` is a leaf.** It includes nothing of inkwell's. A helper that reaches back up the tree
  is how a utility becomes a layer nobody can move, which is the exact mistake this repository
  exists to undo.
- **No threads.** Everything is the one epoll loop, and anything that would block gets a
  descriptor instead — a timerfd, a signalfd, a pipe from a forked child.
- **A blocking call is a bug.** `getaddrinfo()` has no non-blocking form, so the resolver forks;
  a TLS handshake reports `-EAGAIN` all the way up rather than waiting. Nothing here may sit
  between two frames of somebody's UI.
- **A new directory under `src/` needs an entry in `ALLOWED`** in `scripts/check-layers.py`
  before it will compile clean. That is deliberate: adding an area is a decision about the
  shape of the stack, not a `mkdir`.
- **Nothing in here names an application.** A comment may say "a peer" or "a protocol"; the
  moment it says "the radio", the code has learned something it has no business knowing.

## Building

Linux only — `epoll`, `timerfd`, `signalfd`, `eventfd`. There is no portable fallback and there
is not meant to be one.

```bash
make test        # Debug build + ctest - the default verify step, and what CI runs
make debug       # build only
make format      # clang-format all tracked .c/.h (needs clang-format 18)
```

`make test` runs the unit suite and `scripts/check-layers.py`. Run it before every push.

Sanitizers: `cmake -S . -B build -DINKWELL_ENABLE_ASAN=ON -DINKWELL_ENABLE_UBSAN=ON`. CI runs
the suite under gcc, under clang, and under both sanitizers.

### Using it

Vendor it as a submodule and `add_subdirectory()` it; it builds as a consumer without owning the
parent's warning flags or building its own tests.

```cmake
add_subdirectory(third_party/inkwell)
target_link_libraries(your_app PRIVATE inkwell::inkwell)
```

### Tests

One binary with a name filter, not per-test CTest entries. Cases live in `tests/suites/<area>.c`
and **register themselves** — write one with `INKWELL_TEST_CASE(name, category)` and it runs. A
new suite file goes in `INKWELL_TEST_SUITES` in `tests/CMakeLists.txt`.

```bash
./build/tests/inkwell_tests --list
./build/tests/inkwell_tests --filter loop
./build/tests/inkwell_tests --suite codec_zip
```

## Status

Early. The foundation, the loop, the leaf codecs and the resolver are here. Still to come out
of mesh-client, roughly in the order the dependencies allow:

- **the rest of `net/`** — non-blocking TCP connect, Mbed TLS on the loop, the HTTP client that
  drives the codec over a socket, and Mozilla's CA roots compiled in. Deferred only because TLS
  wants the Mbed TLS submodule wired up, which is a slice of its own.
- **`bt/`** — a BlueZ GATT client over D-Bus: discovery, bonding, an `org.bluez.Agent1` that can
  answer a PIN prompt from inside the application, characteristic reads and writes, all
  asynchronous on the loop. About 3,200 lines, of which exactly four are Meshtastic-specific.
- **`io/`** — serial ports, USB device enumeration from sysfs, and USB mass-storage mounting.
- **`store/`** — the key/value file and the append-only log that let an application remember
  things across runs.

## Licence

MIT. See [LICENSE](LICENSE).
