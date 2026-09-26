#!/usr/bin/env python3
"""Check that the records agree with each other and with the code.

    support/trace_check.py            exit 1 on any mismatch
    support/trace_check.py --counts   also print the summary counts

Every requirement ID the code, tests and living documents cite must exist in
REQUIREMENTS.md, and every DD in DECISIONS.md. Every requirement that is not
withdrawn must have a row in TRACEABILITY.md, every test a row names must
exist, and the summary table must count the rows. A function named in
backticks in a living document must exist somewhere in the tree, and a code
comment's pointer to a document section must land on a heading.

The review, its prompts and the task list are not checked for IDs: the review
cites what the code used to say, and the task list names requirements that do
not exist yet.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CODE_DIRS = ["src", "boards", "sim", "test", "www", "support"]
CODE_EXT = (".c", ".h", ".js", ".py")
SKIP_DIRS = {"node_modules", "test-results", "playwright-report", "lua-5.4", "third_party"}

# Cited IDs are checked here. Each is a record of the current code.
ID_DOCS = ["REQUIREMENTS.md", "TRACEABILITY.md", "DECISIONS.md", "IMPLEMENTATION.md",
           "docs/flight_states.md", "docs/code_review_2026-09-24_resolution.md",
           "test/README.md", "support/README.md"]

# Functions named in these must exist: they describe the code as it is.
FUNC_DOCS = ["IMPLEMENTATION.md", "TRACEABILITY.md", "docs/flight_states.md",
             "test/README.md", "support/README.md"]

# Named in the living documents, defined outside this tree.
EXTERNAL_FUNCS = {
    "lfs_file_sync", "lfs_file_close", "lfs_mount", "lfs_format", "time_us_32",
    "time_us_64", "sleep_ms", "powf", "multicore_launch_core1", "flash_range_erase",
    "flash_range_program", "tud_task", "sys_check_timeouts", "watchdog_reboot",
    "picotool", "spin_locks_reset", "mutex_enter_blocking",
}

ID_RE = re.compile(r"\b([A-Z]{2,6}(?:-[A-Z]{2,7})?)-(\d{1,3})(?:\.\.(\d{1,3}))?\b")
DEF_RE = re.compile(r"^- \*\*([A-Z]{2,6}(?:-[A-Z]{2,7})?-\d{1,3})\*\*:\s*(.*)$")
DD_DEF_RE = re.compile(r"^###\s+(DD-\d{3})\b")
TEST_DEF_RE = re.compile(r"\bvoid\s+(test_[A-Za-z0-9_]+)\s*\(")
TEST_REF_RE = re.compile(r"(?<![/\w.])(test_[A-Za-z0-9_]*[A-Za-z0-9])(?!\.[a-z])(\*)?((?:(?:/|\.\.|, )\d{1,3})*)")
FUNC_REF_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)\(\)`")
SECTION_REF_RE = re.compile(r"See ([A-Z_]+\.md) \"([^\"]+)\"")


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def code_files():
    for d in CODE_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, d)):
            dirnames[:] = [x for x in dirnames if x not in SKIP_DIRS]
            for fn in filenames:
                if fn.endswith(CODE_EXT):
                    yield os.path.relpath(os.path.join(dirpath, fn), ROOT)


def expand(family, lo, hi):
    width = len(lo)
    if hi is None:
        return [f"{family}-{lo}"]
    return [f"{family}-{n:0{width}d}" for n in range(int(lo), int(hi) + 1)]


def requirements():
    reqs, withdrawn = {}, set()
    for line in read("REQUIREMENTS.md").splitlines():
        m = DEF_RE.match(line.strip())
        if m:
            reqs[m.group(1)] = m.group(2)
            if m.group(2).startswith("Withdrawn"):
                withdrawn.add(m.group(1))
    return reqs, withdrawn


def decisions():
    return {m.group(1) for line in read("DECISIONS.md").splitlines() for m in [DD_DEF_RE.match(line)] if m}


def trace_rows():
    rows = []
    for line in read("TRACEABILITY.md").splitlines():
        if not line.startswith("| ") or line.startswith("| Req") or line.startswith("| Status"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 4 or set(cells[0]) <= set("-"):
            continue
        rows.append(cells)
    return rows


def test_names():
    names = set()
    for f in code_files():
        if f.startswith("test" + os.sep) and f.endswith(".c"):
            names.update(TEST_DEF_RE.findall(read(f)))
    return names


def test_exists(name, star, names):
    if star:
        return any(n.startswith(name) for n in names)
    return any(n == name or n.startswith(name + "_") for n in names)


def expand_test_ref(base, suffix):
    """test_BRN_01..08, test_FLT_LAUNCH_08/09 and test_HTTP_08, 09 name several."""
    out = [base]
    if not suffix:
        return out
    m = re.match(r"^(.*_)(\d+)$", base)
    if not m:
        return out
    stem, first = m.group(1), m.group(2)
    width = len(first)
    for sep, num in re.findall(r"(/|\.\.|, )(\d{1,3})", suffix):
        if sep == "..":
            out += [f"{stem}{n:0{width}d}" for n in range(int(first) + 1, int(num) + 1)]
        else:
            out.append(f"{stem}{num}")
    return out


def main():
    problems = []
    reqs, withdrawn = requirements()
    dds = decisions()
    families = {r.rsplit("-", 1)[0] for r in reqs}

    # Cited IDs exist.
    sources = list(code_files()) + [d for d in ID_DOCS if os.path.exists(os.path.join(ROOT, d))]
    for path in sources:
        for lineno, line in enumerate(read(path).splitlines(), 1):
            for fam, lo, hi in ID_RE.findall(line):
                if fam == "DD":
                    for dd in expand(fam, lo, hi or None):
                        if dd not in dds:
                            problems.append(f"{path}:{lineno}: {dd} is not in DECISIONS.md")
                elif fam in families:
                    for rid in expand(fam, lo, hi or None):
                        if rid not in reqs:
                            problems.append(f"{path}:{lineno}: {rid} is not in REQUIREMENTS.md")

    # Every live requirement has a row; every named test exists.
    rows = trace_rows()
    traced = set()
    names = test_names()
    for cells in rows:
        for fam, lo, hi in ID_RE.findall(cells[0]):
            traced.update(expand(fam, lo, hi or None))
        for base, star, suffix in TEST_REF_RE.findall(cells[2]):
            for t in expand_test_ref(base, suffix):
                if not test_exists(t, star, names):
                    problems.append(f"TRACEABILITY.md: {cells[0]} names {t}{star}, which no test defines")
    for rid in sorted(set(reqs) - withdrawn - traced):
        problems.append(f"TRACEABILITY.md: no row for {rid}")

    # The summary counts the rows.
    counts = {"verified": 0, "hw": 0, "gap": 0, "missing": 0}
    for cells in rows:
        s = cells[-1]
        if s.startswith("❌"):
            counts["missing"] += 1
        elif s.startswith("⚠"):
            counts["gap"] += 1
        elif s.startswith("✅") and "HW" in s:
            counts["hw"] += 1
        elif s.startswith("✅"):
            counts["verified"] += 1
    summary = read("TRACEABILITY.md").split("## Summary", 1)
    if len(summary) < 2:
        problems.append("TRACEABILITY.md: no Summary section")
    else:
        stated = {}
        for line in summary[1].splitlines():
            m = re.match(r"^\|\s*(✅|⚠️|❌)([^|]*)\|\s*(\d+)", line)
            if not m:
                continue
            key = {"❌": "missing", "⚠️": "gap"}.get(m.group(1))
            if key is None:
                key = "hw" if "HW" in m.group(2) else "verified"
            stated[key] = int(m.group(3))
        for k, v in counts.items():
            if stated.get(k) != v:
                problems.append(f"TRACEABILITY.md: summary says {stated.get(k)} {k}, the tables have {v}")

    # Functions the living documents name exist.
    defined = set()
    for f in code_files():
        defined.update(re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", read(f)))
    for doc in FUNC_DOCS:
        for lineno, line in enumerate(read(doc).splitlines(), 1):
            for fn in FUNC_REF_RE.findall(line):
                if fn not in defined and fn not in EXTERNAL_FUNCS:
                    problems.append(f"{doc}:{lineno}: `{fn}()` is not in the tree")

    # A comment's pointer to a section lands on a heading.
    for f in code_files():
        for lineno, line in enumerate(read(f).splitlines(), 1):
            for doc, section in SECTION_REF_RE.findall(line):
                if not os.path.exists(os.path.join(ROOT, doc)):
                    problems.append(f"{f}:{lineno}: points to {doc}, which does not exist")
                    continue
                heads = [h.lstrip("#").strip() for h in read(doc).splitlines() if h.startswith("#")]
                if section not in heads:
                    problems.append(f"{f}:{lineno}: points to {doc} \"{section}\", which has no such heading")

    for p in problems:
        print(p)
    if "--counts" in sys.argv:
        print(f"rows: {counts['verified']} verified, {counts['hw']} hardware, "
              f"{counts['gap']} not directly verified, {counts['missing']} not implemented")
    print(f"trace_check: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
