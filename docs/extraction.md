# What is left to extract

inkwell exists because mesh-client turned out to be two programs in one repository: an
application that knows what a node and a channel are, and a platform that knows what a socket
and a signal are. The second one keeps being useful to the first one's successors, and this page
is the running account of which parts of it are still on the wrong side of the line.

It is a map, not a promise. A row here is a candidate with its evidence, and the evidence is what
decides whether it moves - not the order of the table.

## The test a candidate has to pass

Three questions, in this order.

1. **Does it name anything from the application?** `grep -h '#include "' <file>` is most of the
   test; the rest is reading the prose, because a comment that says "the radio" is a component
   that will be wrong in the next application even if it compiles. A candidate that fails this
   is not ready - find the seam first.
2. **Is there a seam, or only a wrapper?** The good extractions here each left a two-line
   wrapper behind: `codec/png.h` decodes a PNG of a stated size, and the application's tile
   decoder is that call with 256 in it. The bad version of the same move is dragging the tile
   size down here and calling it general.
3. **Do its tests come with it?** A component that arrives without the cases that held it is a
   downgrade however clean the diff looks. If the cases live inside a larger suite, slice out
   the ones that belong to the component and leave the rest behind.

## Already down here

| What | Was | Seam left behind |
|---|---|---|
| `base/version.h` | `mesh/core/version.h` | which version *this build* is, and whether it was stamped by a release - a compile definition only a build system can answer |
| `codec/mqtt.h` | `mesh/proto/mqtt_packet.h` | the topic derivation, which is a compatibility surface with one firmware rather than a wire format |
| `codec/inflate.h` | `mesh/utils/inflate.h` | nothing; it was already general |
| `codec/png.h` | `mesh/map/tile_image.h` | the tile-sized wrapper, and the two static buffers a 256-square decode needs |
| `runtime/crash.h` | `mesh/utils/crash.h` | the product's name, its issues URL, its note labels, and the sentence about what *its* log may contain |

## Next, in the order the dependencies allow

### 1. The network stack

`tls_client`, `fetch` and `mqtt_proxy` are one tranche because they stand on each other, and all
three are already thin against the application.

| Candidate | Lines | Application includes | What the seam is |
|---|---|---|---|
| `src/core/net/tls_client.c` | 496 | 2 | the CA roots. A generated `ca_roots.c` is a *policy*, and the application should hand over a bundle rather than inherit one |
| `src/core/net/fetch.c` | 870 | 3 | the User-Agent, which names the product and its version |
| `src/core/net/mqtt_proxy.c` | 1142 | 4 | the string ids it reports errors as - see **Errors, not string ids** below |

`codec/mqtt.h` is already here, so `mqtt_proxy` has nothing left to lose but its vocabulary.
Mbed TLS would become inkwell's dependency, which is the real decision in this tranche: it is a
submodule and a config directory, and it is the first optional dependency inkwell would carry.

### 2. The stream link, then the transports

`src/transport/stream_link.c` is 316 lines and includes its own header and `inkwell/base/log.h`
and nothing else. It is a buffered read/write link on a descriptor driven by the loop, which is
as general as anything already here. It goes to `net/`.

The transports above it - serial, TCP - are a different matter. They are ordinary socket and
termios work wrapped in the Meshtastic transport registry and reporting through the string
catalog, so extracting them means answering the errors question below first. The framing they
use (`src/proto/stream_framing.c`) is Meshtastic's and stays.

### 3. BlueZ

`src/transport/ble/bluez_client.c` is 2,980 lines and includes exactly one application header -
its own. Everything in it is D-Bus, BlueZ object paths, GATT characteristics and a pairing agent.
It is the largest single piece of platform still sitting in the application, and for a handheld
OS it is the most valuable: nothing else in the tree is a reusable Bluetooth stack.

The seam is already described in mesh-client's own CLAUDE.md: the general client comes down here,
and a lookup by one well-known service UUID stays up there. `include/mesh/transport/ble_bluez.h`
is the file to split - it carries the generic client next to five Meshtastic UUID constants and a
struct named after them.

### 4. Binary formats

`uf2.c` (157 lines) and `esp_image.c` (79) each include only their own header. UF2 is
Microsoft's and the ESP image header is Espressif's; neither has anything to do with a mesh. They
are small, so they are not urgent - but they are `codec/` by every test above, and a platform
that flashes a peripheral wants them.

### 5. Storage, once there is a seam

`src/ui/store/store_file.c` (1,522 lines) is two things wound together: a key/value file on a
card with an append-only record codec, and a model of what a Meshtastic client remembers. The
first is platform and every application wants it; the second is not. Six application includes say
it is not ready. The same is true of `store_archive.c` (906 lines, 3).

This is the one worth doing properly rather than quickly, because "a small durable store" is a
thing an OS offers and a thing every application then depends on. Find the codec seam first -
probably a record reader/writer over a file, with the field table staying up in the application -
and move that.

## Two questions that block several rows

### Errors, not string ids

Several candidates - the transports, `mqtt_proxy` - report failure by handing back a
`MESH_STR_*` id for the application's catalog to translate. inkwell has no catalog and should not
grow one: a platform layer that owns the words is a platform layer every application argues with.

The rule this repo already states is the answer: **functions return 0 or a negative errno, and a
refusal a caller must distinguish gets its own enum.** So `net/tcp.h` returns
`INKWELL_TCP_UNREACHABLE` and the application maps that to whatever it wants to say, in whatever
language. It is more work than moving the id, and it is the difference between a layer and a
library with a dialect.

Do this per component as it moves, not as a sweep beforehand.

### Where the UI toolkit ends

inkcell stands on inkwell, so anything both would want belongs here. `codec/png.h` is the first
case of that and was easy - a decoder is bytes in, bytes out, and a UI toolkit that owned image
decoding would own it for everything above it too. Expect the same argument about fonts, and
expect it to go the other way: a glyph table is a typeface, which is a design decision, which is
inkcell's.

## What must not come down here

- **Anything that names the application's domain**, in code or in a comment. Say "a peer", "a
  transport", "a filter". The day a comment in here says "the radio" this stopped being a
  platform layer.
- **A word a user reads.** See "Errors, not string ids" above.
- **A threaded anything.** Whatever would block gets a descriptor and a callback.
- **A policy dressed as a mechanism.** A CA bundle, a retry schedule, a cache budget: the
  mechanism comes here and the number stays up there.
