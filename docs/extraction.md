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
   test, **and `<file>` is the source *and its header*.** The rest is reading the prose, because
   a comment that says "the radio" is a component that will be wrong in the next application
   even if it compiles. A candidate that fails this is not ready - find the seam first.

   The emphasis is there because reading only the source got a row on this page wrong for a
   whole tranche. `stream_link.c` includes one application header - its own - and looks clean;
   that header includes two more, and the count that mattered was never in the file being
   grepped. A component's dependencies are what a caller has to compile against, not what its
   first translation unit spells out. Check both, and check what the source actually *calls*
   across the line, which is the claim the includes are only evidence for.
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
| `net/reason.h` | nothing - it is new | the table from a reason to a sentence, which is the application's whole half of this |
| `net/stream.h` | `mesh/transport/stream_link.h` | the frame parser, the session it feeds, and the two numbers that size the outbound queue |
| `net/tls.h` | `mesh/core/tls_client.h` | which roots to trust, and where they came from - a generated table compiled into a binary is one product's answer to shipping without a certificate store |
| `net/fetch.h` | `mesh/core/fetch.h` | the product's name and version, sent as `User-Agent`, and every state machine that decides what to do with what came back |
| `ble/central.h` | `mesh/transport/ble_bluez.h` | the five service and characteristic UUIDs one firmware publishes, the lookup of all four at once, and every policy about when to scan, connect, pair and give up |

## Next, in the order the dependencies allow

### 1. The network stack

**`tls_client` and `fetch` have come down as `net/tls.h` and `net/fetch.h`.** Both went the way
`codec/png.h` did: the mechanism came here and the number stayed up there. For TLS the number was
the trust anchors, which are now registered by the application rather than linked against; for
the fetcher it was the `User-Agent`, which is the product's name and never was this layer's to
know. Neither header had an application include to begin with - the seams were both one line in
a source file, which is the shape a candidate should be in before it moves at all.

Mbed TLS came with them, and it is worth being accurate about what that cost. It is *not*
inkwell's first dependency: Wuffs has been here since `codec/png.h`, with the same
`third_party/x` and `third_party/x-config` shape. What is new is the first **submodule** - the
first thing a clone has to go and get - and that is why it is optional by presence rather than
by a flag, and why CI carries a job that builds without it. A platform layer may have a
dependency; it may not make everyone who clones it pay for one.

| Candidate | Lines | Application includes | What the seam is |
|---|---|---|---|
| `src/core/net/mqtt_proxy.c` | 1142 | 4 | the string ids it reports errors as - `net/reason.h` is what it reports instead |

`codec/mqtt.h` is already here and the vocabulary question is answered, so `mqtt_proxy` is now
the one row left in this tranche.

**One thing was deliberately left undone.** `enum inkwell_fetch_outcome` came down unchanged, and
its `NETWORK` member folds four things `net/reason.h` can tell apart - an unknown host, a lookup
that failed, an unreachable address, a connection that closed - into one. That is the same
duplication `net/reason.h` was written to remove, and a caller that wants to say "no such host"
rather than "could not be reached" cannot. Folding it is a behaviour change to every caller's
sentences, so it did not belong in a move; it is worth doing on its own, with the call sites in
view.

### 2. The transports, now that the stream under them has gone

**The stream link has come down as `net/stream.h`.** It went as a seam rather than as a file
move, which is what the re-measurement above said it would be: the general form is a byte
stream, not a frame link. Bytes arrive as bytes and go out as bytes, and the two numbers that
used to size its outbound queue - one protocol's largest message, one application's patience -
are the caller's storage now. What stayed behind is the frame parser, the session it feeds, and
a `struct mesh_stream_link` that is those two things over an `inkwell_stream`.

The transports over it - serial, TCP - are the next step and a larger one. They are ordinary
socket and termios work wrapped in the Meshtastic transport registry. The errors question that
used to block them is answered (`net/reason.h`) and the TCP link already reports that way; what
is left is the registry, which is the application's, and the device discovery underneath it,
which is not:

| Candidate | What is general | What stays |
|---|---|---|
| `src/transport/serial/serial_usb.c` | the sysfs scan, the termios setup, the DTR assert | which USB ids are a radio |
| `src/transport/tcp/tcp_transport.c` | the connect-with-a-deadline over `net/resolve` and `net/stream` | the registry, the handshake, the auto-connect policy |

Neither is urgent and neither is blocked. A handheld OS wants "open the serial device at this
path with these settings" long before it wants a transport registry.

### 3. BlueZ - done, as `ble/central.h`

The BlueZ client came down as a new area, `ble/`, and it came down as an interface rather than as
a file move. The old header named a device by its BlueZ object path, and the application built
those paths itself (`<adapter>/dev_AA_BB_...`) - which is one stack's vocabulary leaking through
every call. `ble/central.h` names a peripheral by its *address* and a characteristic by an opaque
*handle*, so the object path is now a detail of `src/ble/bluez.c`.

That is what makes a second backend possible at all. macOS's CoreBluetooth has no object paths
and never reveals a hardware address - it names a peripheral by a UUID of its own - so the
interface had to stop assuming either before one could be written. `central.c` holds everything
that is the same on every stack (the argument checks, the request bookkeeping and deadlines, the
test mock), and a backend is linked in at build time: `bluez.c` where libdbus-1 was found,
`corebluetooth.m` on macOS, `none.c` everywhere else.

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

## The two questions that blocked several rows

### Errors, not string ids - answered

Several candidates - the transports, `mqtt_proxy` - report failure by handing back a
`MESH_STR_*` id for the application's catalog to translate. inkwell has no catalog and should not
grow one: a platform layer that owns the words is a platform layer every application argues with.

The answer is `net/reason.h`: **a reason and a number, and the application turns the pair into a
sentence.** Reading every failure site in `tcp_transport.c` and `mqtt_proxy.c` is what settled
it, because each one's arguments turn out to be one of only three things:

| What the call site passes today | Where it comes from instead |
|---|---|
| `state->target`, `proxy->host` | the caller passed it in and still has it |
| `strerror(error)`, `strerror(errno)` | an `int` |
| `(unsigned)code`, a TLS code | an `int` |

So nothing that crosses is a word, and `struct inkwell_net_failure` is two fields and no buffer.
The sentences were already half-untranslated - `LINK_TCP_UNREACHABLE` is `"%.24s: %.20s"` with
`strerror()` formatted into it - so carrying the number loses nothing and stops pretending.

**Two enums, not one.** `enum inkwell_net_reason` is the part every transport shares: getting to
a host. What happens *after* the connection is up is the protocol's, and stays with the protocol
- MQTT's CONNACK refusals are `codec/mqtt.h`'s business, not `net/`'s. The evidence for the
split is in the catalog: `LINK_TCP_UNKNOWN_HOST` and `LINK_MQTT_UNKNOWN_HOST` are the *same
English sentence written twice*, and so are the lookup-failed, unreachable and timeout pairs.
Four duplicate strings, translated twice, because the reason had nowhere to live.

**Nothing collapses on the way down.** Where the layer can tell two failures apart it keeps them
apart, even where every application today says one thing about both - `LOOKUP_TIMED_OUT` is not
folded into `LOOKUP_FAILED`. Merging is a decision about words, so it belongs in the table that
produces words. That table is the application's; in mesh-client it is the transport's
`take_error`.

**Convert in the component, in its own commit, just before it moves.** Not as a sweep
beforehand, and not in the same diff as the move: the MQTT extraction mixed a rename into a
relocation and hid 26 wrong names inside it, and an errno conversion folded into a file move is
that trap with more surface. One diff that changes behaviour with the tests still in place, then
one diff that is a move.

### Where the UI toolkit ends

inkcell stands on inkwell, so anything both would want belongs here. `codec/png.h` is the first
case of that and was easy - a decoder is bytes in, bytes out, and a UI toolkit that owned image
decoding would own it for everything above it too. Expect the same argument about fonts, and
expect it to go the other way: a glyph table is a typeface, which is a design decision, which is
inkcell's.

**The string catalog is the case this turns on, and the answer is that it stays in inkcell.**
The mechanism is not a UI concern by any test above - a table of ids, a locale, a plural rule
and a checked format string are a text service, and a headless daemon that wanted translated
output would want all four. By the `codec/png.h` argument it should come down.

It does not come down, for one reason: **"inkwell has no catalog" is worth more as a structural
impossibility than as a discipline.** The rule at the top of this page - no word a user reads -
is kept today by nothing but people remembering it. If there is no catalog to name an id in,
nothing here *can* report a word, and that property holds across every application nobody has
written yet. The errors question above was the only thing that made the rule expensive, and
`net/reason.h` is what made it cheap.

Revisit when something that does not draw actually needs translated text. Until then this is
speculative generality with a real cost.

## What must not come down here

- **Anything that names the application's domain**, in code or in a comment. Say "a peer", "a
  transport", "a filter". The day a comment in here says "the radio" this stopped being a
  platform layer.
- **A word a user reads.** See "Errors, not string ids" above.
- **A threaded anything.** Whatever would block gets a descriptor and a callback.
- **A policy dressed as a mechanism.** A CA bundle, a retry schedule, a cache budget: the
  mechanism comes here and the number stays up there.
