#!/usr/bin/env python3
"""The version number rules, in one place.

    MAJOR.MINOR.PATCH

A patch bump is automatic: CI cuts one from every green push to main, so
the released version and the code on main never drift apart. Major and
minor bumps are deliberate, and when one happens everything below it goes
to zero:

    major   X.Y.Z -> (X+1).0.0
    minor   X.Y.Z -> X.(Y+1).0
    patch   X.Y.Z -> X.Y.(Z+1)

Which is also the rule worth ENFORCING, because the failure mode is quiet:
editing VERSION from 2.2.7 to 2.3.7 looks like a minor bump and leaves a
patch number that means nothing, and nothing downstream would complain.
is_legal() is what CI checks, so a version that breaks the rules fails the
build rather than shipping.

    ./support/version.py --next patch            next patch from VERSION
    ./support/version.py --next minor            next minor from VERSION
    ./support/version.py --set 3.0.0             write it, if the step is legal
    ./support/version.py --check 2.2.7 2.3.0     is that step legal?
    ./support/version.py --selftest              the rules, exhaustively
"""
import argparse
import os
import re
import sys

VERSION_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "VERSION")

LEVELS = ("major", "minor", "patch")


def parse(text):
    """(major, minor, patch) from a version string, or None if it is not one.

    Strict on purpose: a leading "v", a four-part version or a build suffix
    are all things that would go on to be compared numerically with
    something that is not the same shape.
    """
    if text is None:
        return None
    m = re.fullmatch(r"\s*(\d+)\.(\d+)\.(\d+)\s*", str(text))
    if not m:
        return None
    return tuple(int(g) for g in m.groups())


def format_version(parts):
    return "%d.%d.%d" % parts


def next_version(current, level):
    """The version after a bump at `level`, with everything below it zeroed."""
    parts = parse(current)
    if parts is None:
        raise ValueError("not a version: %r" % (current,))
    if level not in LEVELS:
        raise ValueError("level must be one of %s" % (", ".join(LEVELS),))
    major, minor, patch = parts
    if level == "major":
        return format_version((major + 1, 0, 0))
    if level == "minor":
        return format_version((major, minor + 1, 0))
    return format_version((major, minor, patch + 1))


def step_level(old, new):
    """Which bump takes `old` to `new`, "none" if they are equal, or None.

    None means no single legal bump gets there -- a skipped patch, a minor
    bump that kept its patch number, a move backwards.
    """
    if parse(old) is None or parse(new) is None:
        return None
    if parse(old) == parse(new):
        return "none"
    for level in LEVELS:
        if parse(next_version(old, level)) == parse(new):
            return level
    return None


def is_legal(old, new):
    return step_level(old, new) is not None


def read_version(path=VERSION_FILE):
    with open(path) as f:
        return f.read().strip()


def write_version(value, path=VERSION_FILE):
    parts = parse(value)
    if parts is None:
        raise ValueError("not a version: %r" % (value,))
    with open(path, "w") as f:
        f.write(format_version(parts) + "\n")


def selftest():
    cases_next = [
        ("2.2.7", "patch", "2.2.8"),
        ("2.2.7", "minor", "2.3.0"),   # patch zeroed
        ("2.2.7", "major", "3.0.0"),   # minor and patch zeroed
        ("0.0.0", "patch", "0.0.1"),
        ("0.9.9", "minor", "0.10.0"),  # no decimal carry: 9 -> 10
        ("9.9.9", "major", "10.0.0"),
        ("2.2.754", "minor", "2.3.0"), # a large patch still zeroes
    ]
    legal = [
        ("2.2.7", "2.2.8", "patch"),
        ("2.2.7", "2.3.0", "minor"),
        ("2.2.7", "3.0.0", "major"),
        ("2.2.7", "2.2.7", "none"),
    ]
    illegal = [
        ("2.2.7", "2.3.7"),   # minor bump that kept its patch
        ("2.2.7", "2.3.1"),
        ("2.2.7", "3.1.0"),   # major bump that kept its minor
        ("2.2.7", "3.0.1"),   # major bump that kept a patch
        ("2.2.7", "2.2.9"),   # skipped a patch
        ("2.2.7", "2.2.6"),   # backwards
        ("2.2.7", "2.1.0"),   # minor backwards
        ("2.2.7", "1.0.0"),   # major backwards
        ("2.2.7", "4.0.0"),   # skipped a major
        ("2.2.7", "2.4.0"),   # skipped a minor
        ("2.2.7", "v2.2.8"),  # not a bare version
        ("2.2.7", "2.2"),
        ("2.2.7", "2.2.8.1"),
        ("2.2.7", ""),
        ("2.2.7", None),
    ]

    bad = 0
    for cur, level, want in cases_next:
        got = next_version(cur, level)
        ok = got == want
        bad += not ok
        print("  %s  next(%s, %-5s) = %-8s want %s" % ("pass" if ok else "FAIL", cur, level, got, want))
    for old, new, want in legal:
        got = step_level(old, new)
        ok = got == want
        bad += not ok
        print("  %s  %s -> %-8s legal as %-6s want %s" % ("pass" if ok else "FAIL", old, new, got, want))
    for old, new in illegal:
        got = step_level(old, new)
        ok = got is None
        bad += not ok
        print("  %s  %s -> %-8s rejected%s" % ("pass" if ok else "FAIL", old, new,
                                               "" if ok else " BUT ALLOWED AS %s" % got))

    # Every bump must zero everything below it, for any starting point.
    for major in range(3):
        for minor in range(3):
            for patch in range(3):
                cur = format_version((major, minor, patch))
                if parse(next_version(cur, "major"))[1:] != (0, 0):
                    print("  FAIL  major bump from %s did not zero below" % cur)
                    bad += 1
                if parse(next_version(cur, "minor"))[2] != 0:
                    print("  FAIL  minor bump from %s did not zero the patch" % cur)
                    bad += 1

    print("\n%s" % ("all version rules hold" if not bad else "%d FAILED" % bad))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description="Version number rules")
    ap.add_argument("--next", choices=LEVELS, help="print the next version at this level")
    ap.add_argument("--set", metavar="VERSION", help="write VERSION, if the step from the current one is legal")
    ap.add_argument("--check", nargs=2, metavar=("OLD", "NEW"), help="is that step legal?")
    ap.add_argument("--current", action="store_true", help="print the current version")
    ap.add_argument("--validate", metavar="VERSION", help="exit 0 if that is a well-formed version")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if args.validate is not None:
        if parse(args.validate) is None:
            print("not a version: %r -- want MAJOR.MINOR.PATCH" % (args.validate,), file=sys.stderr)
            return 1
        print(format_version(parse(args.validate)))
        return 0

    if args.check:
        old, new = args.check
        level = step_level(old, new)
        if level is None:
            print("illegal: %s -> %s" % (old, new), file=sys.stderr)
            print("  a bump zeroes everything below it: %s, %s or %s"
                  % (next_version(old, "patch") if parse(old) else "?",
                     next_version(old, "minor") if parse(old) else "?",
                     next_version(old, "major") if parse(old) else "?"), file=sys.stderr)
            return 1
        print(level)
        return 0

    if args.current:
        print(read_version())
        return 0

    if args.next:
        print(next_version(read_version(), args.next))
        return 0

    if args.set:
        current = read_version()
        if not is_legal(current, args.set):
            print("illegal: %s -> %s" % (current, args.set), file=sys.stderr)
            print("  legal from here: %s (patch), %s (minor), %s (major)"
                  % (next_version(current, "patch"), next_version(current, "minor"),
                     next_version(current, "major")), file=sys.stderr)
            return 1
        write_version(args.set)
        print(args.set)
        return 0

    ap.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
