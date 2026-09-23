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
                       (macOS for development: kqueue, and a window instead of the panel)
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
| `net/` | One hostname turned into an address by a child that may block; a non-blocking TCP connector with a deadline; a byte stream over a descriptor; TLS that reports `-EAGAIN` rather than waiting; one HTTPS request; and one bounded MQTT 3.1.1 client with subscriptions, keepalive, and reconnect backoff. All run on the same loop and hold no application policy. |
| `io/` | The USB serial ports the system has - sysfs on Linux, the I/O Registry on macOS - with what the USB tree says about each (a bridge chip or the device's own USB, a mass-storage interface beside it or not), and a tty opened raw and non-blocking for the loop. For a kernel without CDC-ACM, the generic-driver bind and the usbfs line-state request that make a native-USB device talk anyway. |

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

Linux is the target — `epoll`, `timerfd`, `signalfd`, `eventfd`. macOS builds and passes the
same suite as a **development host**: the loop is `kqueue` there, and the timer, the wake and the
signals are the three descriptors `runtime/` hands out instead of the Linux calls, so nothing
above this layer names either system. It exists so the UI above can be worked on in a window
without a device; nothing ships for it, and CI builds and tests it so it does not rot. There is
no third backend and there is not meant to be one.

On macOS the Mbed TLS generator wants a Python with `jinja2` and `jsonschema`; point CMake at one
with `-DPython3_EXECUTABLE=...` if the one on `PATH` does not have them.

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

**TLS is the one optional part.** `net/tls.h` and the HTTPS and MQTT clients that stand on it need
Mbed TLS, which is a submodule at `third_party/mbedtls`. Clone with `--recurse-submodules`, or
`git submodule update --init --recursive` afterwards, and it is compiled in. Without it both
headers still exist and every symbol still links: a session refuses to start with `-ENOTSUP` and
a fetcher reports itself unavailable, so nothing above needs an `#ifdef`. Everything else builds
and tests identically either way, and CI has a job that proves it.

Two things are the application's to supply rather than inherit, both once at startup:

```c
inkwell_tls_set_roots(my_roots, my_root_count);   /* what this product trusts */
inkwell_fetch_set_user_agent("yourthing/1.4.2");  /* what it calls itself */
```

There is no default root set and no fallback. With nothing registered a session is refused
rather than opened unverified — which root store a product uses is a decision about how it ships,
and a layer this far down guessing at it would be an insecure mode arrived at quietly.

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

The platform pieces identified during the extraction from mesh-client are here. The current
boundary and the evidence for everything that stayed in the application are recorded in
[`docs/extraction.md`](docs/extraction.md).

## Licence

MIT. See [LICENSE](LICENSE).
