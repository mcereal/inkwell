#!/usr/bin/env python3
"""Fail when one area of inkwell includes a header from an area it is not allowed to see.

Every `.c` here compiles into one library, so the linker has no opinion about direction:
`src/base/time.c` could include "inkwell/runtime/loop.h" tomorrow and the build would be
delighted. The layering is a rule the compiler cannot see, and this is the check that does.

An area is the directory under `src/` or `include/inkwell/`, so `src/codec/zip.c` and
`include/inkwell/codec/zip.h` are both `codec`. ALLOWED lists, per area, the areas it may
include from; including from its own area is always fine. Both `"inkwell/..."` and
`<inkwell/...>` are read, because both compile.

The direction that matters most is that **base never includes anything**. It is the floor the
whole stack stands on - a clock, a log line, a string helper - and a floor that reaches back up
is how a utility becomes a layer nobody can move. inkcell's utils were exactly that, which is
why they are down here now.

Run it directly, or through `ctest` / `make test`, which is where it will catch somebody.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# What each area may include from, beyond itself. The comment on a line is why the edge exists;
# an edge with no reason to be here is one to delete rather than one to document.
ALLOWED = {
    # The floor. A leaf on purpose: see the docstring.
    "base": set(),
    # Bytes in, bytes out. A codec parses; it does not know what the bytes are for, and it
    # certainly does not schedule anything.
    "codec": {"base"},
    # The loop and the signals that stop it. It logs, and it reads a clock.
    "runtime": {"base"},
    # One hostname, one socket, one session. It runs on the loop and speaks through a codec;
    # it does not know which application asked.
    "net": {"base", "runtime", "codec"},
    # One Bluetooth LE central. Like net/, it runs on the loop and does not know which
    # application asked; unlike net/, it has no bytes to parse, so it needs no codec.
    "ble": {"base", "runtime"},
    # The serial ports the system has, and a tty opened on them. A port is a descriptor the
    # caller hands to the loop or to net/stream.h itself; nothing here schedules anything.
    "io": {"base"},
}

INCLUDE = re.compile(r'^\s*#\s*include\s+["<]inkwell/([a-z0-9_]+)/')


def area_of(path):
    """The area a file belongs to, or None for a file this check has no opinion about."""
    parts = path.relative_to(ROOT).parts
    if parts[0] == "src":
        return parts[1] if len(parts) > 2 else None
    if parts[:2] == ("include", "inkwell"):
        return parts[2] if len(parts) > 3 else None
    return None


def sources():
    for root in (ROOT / "src", ROOT / "include" / "inkwell"):
        if not root.exists():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix in (".c", ".h"):
                yield path


def main():
    problems = []
    for path in sources():
        area = area_of(path)
        if area is None:
            continue
        allowed = ALLOWED.get(area, set()) | {area}
        for number, line in enumerate(path.read_text().splitlines(), start=1):
            match = INCLUDE.match(line)
            if match and match.group(1) not in allowed:
                problems.append(
                    "%s:%d: %s may not include from %s"
                    % (path.relative_to(ROOT), number, area, match.group(1))
                )
            if match is None and re.match(r'^\s*#\s*include\s+["<](mesh|inkcell)/', line):
                problems.append(
                    "%s:%d: inkwell is the bottom of the stack and may not include from %s"
                    % (path.relative_to(ROOT), number, line.split('"')[1].split("/")[0])
                )

    unknown = {area_of(p) for p in sources()} - set(ALLOWED) - {None}
    for area in sorted(unknown):
        problems.append("src/%s/: a new area needs an entry in ALLOWED in this script" % area)

    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        print("\n%d layering violation(s)" % len(problems), file=sys.stderr)
        return 1
    print("layers ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
