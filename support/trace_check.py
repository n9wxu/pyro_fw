#!/usr/bin/env python3
"""Check that the records agree with each other and with the code.

    support/trace_check.py             exit 1 on any mismatch
    support/trace_check.py --counts    also print the summary counts
    support/trace_check.py --selftest  each check, on made-up inputs

Requirement IDs:
  - every ID the code, tests, scripts, workflows and living documents cite
    exists in REQUIREMENTS.md, and every DD in DECISIONS.md;
  - an ID-shaped token whose family is not a requirement family is an error,
    unless the family is an outside standard's (UTF-8, CRC-16, ...);
  - code, tests and TRACEABILITY.md cite no withdrawn requirement, ranges
    (PYR-SAFE-01..03) included.
TRACEABILITY.md:
  - every live requirement has exactly one row, and every row a requirement;
  - every test a row names exists, and every verified (✅, not HW) row names
    at least one;
  - the summary table counts the rows.
Pointers:
  - a function named in backticks in a living document exists in the tree;
  - a comment's `X.md "Heading"` pointer lands on a heading, also when the
    comment wraps it across lines or names several headings;
  - a `docs/....md` path named in a comment exists.

The reviews, their prompts and the task list are not checked for IDs: a
review cites what the code used to say, and the task list names requirements
that do not exist yet.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CODE_DIRS = ["src", "boards", "sim", "test", "www", "support", "scripts", ".github"]
CODE_EXT = (".c", ".h", ".js", ".py", ".pio", ".cmake", ".sh", ".yml")
ROOT_CODE_FILES = ["CMakeLists.txt", "pico_sdk_import.cmake"]
SKIP_DIRS = {"node_modules", "test-results", "playwright-report", "lua-5.4", "third_party"}
SELF = os.path.join("support", "trace_check.py")  # its --selftest cites bad IDs on purpose

# Cited IDs are checked here. Each is a record of the current code.
ID_DOCS = ["REQUIREMENTS.md", "TRACEABILITY.md", "DECISIONS.md", "IMPLEMENTATION.md",
           "docs/flight_states.md", "docs/code_review_2026-09-24_resolution.md",
           "test/README.md", "support/README.md"]

# Where a withdrawn requirement may still be named: its own definition, and
# the decisions that withdrew it.
WITHDRAWN_MAY_APPEAR = {"REQUIREMENTS.md", "DECISIONS.md", "docs/code_review_2026-09-24_resolution.md"}

# ID-shaped tokens from outside standards and part numbers, not requirements.
EXTERNAL_FAMILIES = {"UTF", "CRC", "QFN", "BSD", "SHA", "RFC", "ISO", "IEC", "IEEE"}
# A document whose own records carry another ID scheme.
DOC_FAMILIES = {"docs/code_review_2026-09-24_resolution.md": {"REV"}}

# Functions named in these must exist: they describe the code as it is.
# Each board's theory of operation joins them.
FUNC_DOCS = ["IMPLEMENTATION.md", "TRACEABILITY.md", "docs/flight_states.md",
             "test/README.md", "support/README.md"] + sorted(
    os.path.relpath(os.path.join(d, "THEORY_OF_OPERATION.md"), ROOT)
    for d in (os.path.join(ROOT, "boards", b) for b in os.listdir(os.path.join(ROOT, "boards")))
    if os.path.exists(os.path.join(d, "THEORY_OF_OPERATION.md")))

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
TEST_REF_RE = re.compile(r"(?<![/\w.])(test_[A-Za-z0-9_]*[A-Za-z0-9])(?![A-Za-z0-9_]|\.[a-z])(\*)?((?:(?:/|\.\.|, )\d{1,3})*)")
FUNC_REF_RE = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*)\(\)`")
# X.md "Heading", or X.md "One", "Two" and "Three".
SECTION_REF_RE = re.compile(r"((?:[\w.-]+/)*[\w.-]+\.md),?\s+(\"[^\"\n]+\"(?:\s*(?:,\s*and|,\s*or|,|and|or)\s*\"[^\"\n]+\")*)")
HEADING_RE = re.compile(r"\"([^\"]+)\"")
DOC_PATH_RE = re.compile(r"(?<![\w/.-])(docs/[\w./-]*[\w-]\.md)\b")
COMMENT_PREFIX_RE = re.compile(r"^\s*(?:/\*+|\*+(?!/)|//+|#+|--+)?\s?")


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def code_files():
    for f in ROOT_CODE_FILES:
        if os.path.exists(os.path.join(ROOT, f)):
            yield f
    for d in CODE_DIRS:
        for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, d)):
            dirnames[:] = [x for x in dirnames if x not in SKIP_DIRS]
            for fn in filenames:
                path = os.path.relpath(os.path.join(dirpath, fn), ROOT)
                if (fn.endswith(CODE_EXT) or fn == "CMakeLists.txt") and path != SELF:
                    yield path


def expand(family, lo, hi):
    width = len(lo)
    if hi is None:
        return [f"{family}-{lo}"]
    return [f"{family}-{n:0{width}d}" for n in range(int(lo), int(hi) + 1)]


def parse_requirements(text):
    reqs, withdrawn = {}, set()
    for line in text.splitlines():
        m = DEF_RE.match(line.strip())
        if m:
            reqs[m.group(1)] = m.group(2)
            if m.group(2).startswith("Withdrawn"):
                withdrawn.add(m.group(1))
    return reqs, withdrawn


def decisions():
    return {m.group(1) for line in read("DECISIONS.md").splitlines() for m in [DD_DEF_RE.match(line)] if m}


def parse_trace_rows(text):
    rows = []
    for line in text.splitlines():
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


def is_verified(status):
    return status.startswith("✅") and "HW" not in status


# ── The checks, each on text, so --selftest can feed them ─────────────


def id_problems(path, text, reqs, withdrawn, dds, check_withdrawn):
    families = {r.rsplit("-", 1)[0] for r in reqs}
    allowed = EXTERNAL_FAMILIES | DOC_FAMILIES.get(path, set())
    problems = []
    for lineno, line in enumerate(text.splitlines(), 1):
        for fam, lo, hi in ID_RE.findall(line):
            ids = expand(fam, lo, hi or None)
            if fam == "DD":
                problems += [f"{path}:{lineno}: {dd} is not in DECISIONS.md" for dd in ids if dd not in dds]
            elif fam in families:
                for rid in ids:
                    if rid not in reqs:
                        problems.append(f"{path}:{lineno}: {rid} is not in REQUIREMENTS.md")
                    elif check_withdrawn and rid in withdrawn:
                        problems.append(f"{path}:{lineno}: {rid} is withdrawn")
            elif fam not in allowed:
                problems.append(f"{path}:{lineno}: {fam}-{lo} is not a requirement family")
    return problems


def row_problems(rows, reqs, withdrawn, names):
    problems = []
    traced = {}
    for cells in rows:
        for fam, lo, hi in ID_RE.findall(cells[0]):
            for rid in expand(fam, lo, hi or None):
                if rid in traced:
                    problems.append(f"TRACEABILITY.md: {rid} has two rows")
                traced[rid] = cells
        named = 0
        for base, star, suffix in TEST_REF_RE.findall(cells[2]):
            for t in expand_test_ref(base, suffix):
                named += 1
                if not test_exists(t, star, names):
                    problems.append(f"TRACEABILITY.md: {cells[0]} names {t}{star}, which no test defines")
        if is_verified(cells[-1]) and named == 0:
            problems.append(f"TRACEABILITY.md: {cells[0]} is ✅ but names no test")
    for rid in sorted(set(reqs) - withdrawn - set(traced)):
        problems.append(f"TRACEABILITY.md: no row for {rid}")
    return problems


def comment_text(text):
    """The file with each line's comment marker removed and the lines joined,
    and the line each character came from."""
    parts, line_of = [], []
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = COMMENT_PREFIX_RE.sub("", line, count=1).rstrip()
        stripped = re.sub(r"\s*\*/$", "", stripped)
        parts.append(stripped + " ")
        line_of += [lineno] * (len(stripped) + 1)
    return "".join(parts), line_of


def headings(doc_text):
    return {" ".join(h.lstrip("#").split()) for h in doc_text.splitlines() if h.startswith("#")}


def pointer_problems(path, text, find_doc):
    """find_doc(path, doc) gives the document's text, or None."""
    problems = []
    joined, line_of = comment_text(text)
    for m in SECTION_REF_RE.finditer(joined):
        doc = m.group(1)
        lineno = line_of[m.start()]
        doc_text = find_doc(path, doc)
        if doc_text is None:
            problems.append(f"{path}:{lineno}: points to {doc}, which does not exist")
            continue
        heads = headings(doc_text)
        for section in HEADING_RE.findall(m.group(2)):
            section = " ".join(section.split())
            if section not in heads:
                problems.append(f"{path}:{lineno}: points to {doc} \"{section}\", which has no such heading")
    return problems


def doc_path_problems(path, text, exists):
    problems = []
    for lineno, line in enumerate(text.splitlines(), 1):
        for doc in DOC_PATH_RE.findall(line):
            if not exists(doc):
                problems.append(f"{path}:{lineno}: names {doc}, which does not exist")
    return problems


def find_doc_in_tree(path, doc):
    """The document beside the file first -- a board's THEORY_OF_OPERATION.md
    -- then from the top of the tree."""
    for candidate in (os.path.join(os.path.dirname(path), doc), doc):
        if os.path.exists(os.path.join(ROOT, candidate)):
            return read(candidate)
    return None


def main():
    problems = []
    reqs, withdrawn = parse_requirements(read("REQUIREMENTS.md"))
    dds = decisions()

    code = list(code_files())
    docs = [d for d in ID_DOCS if os.path.exists(os.path.join(ROOT, d))]
    for path in code + docs:
        problems += id_problems(path, read(path), reqs, withdrawn, dds, path not in WITHDRAWN_MAY_APPEAR)

    rows = parse_trace_rows(read("TRACEABILITY.md"))
    problems += row_problems(rows, reqs, withdrawn, test_names())

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

    defined = set()
    for f in code:
        defined.update(re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", read(f)))
    for doc in FUNC_DOCS:
        for lineno, line in enumerate(read(doc).splitlines(), 1):
            for fn in FUNC_REF_RE.findall(line):
                if fn not in defined and fn not in EXTERNAL_FUNCS:
                    problems.append(f"{doc}:{lineno}: `{fn}()` is not in the tree")

    for f in code:
        text = read(f)
        problems += pointer_problems(f, text, find_doc_in_tree)
        problems += doc_path_problems(f, text, lambda d: os.path.exists(os.path.join(ROOT, d)))

    for p in problems:
        print(p)
    if "--counts" in sys.argv:
        print(f"rows: {counts['verified']} verified, {counts['hw']} hardware, "
              f"{counts['gap']} not directly verified, {counts['missing']} not implemented")
    print(f"trace_check: {len(problems)} problem(s)")
    return 1 if problems else 0


# ── --selftest ─────────────────────────────────────────────────────────

SELFTEST_REQS = """
- **PYR-SAFE-01**: Live.
- **PYR-SAFE-02**: Withdrawn (DD-001).
- **PYR-SAFE-03**: Live.
- **FLT-LAND-01**: Live.
"""

SELFTEST_DOC = """# Theory
## The ladder
## Two words here
"""


def selftest():
    reqs, withdrawn = parse_requirements(SELFTEST_REQS)
    dds = {"DD-001"}
    names = {"test_PYR_SAFE_01_fires_once", "test_FLT_LAND_01_rests"}

    def ids(text, path="src/x.c", check_withdrawn=True):
        return id_problems(path, text, reqs, withdrawn, dds, check_withdrawn)

    def rows(table):
        return row_problems(parse_trace_rows(table), reqs, withdrawn, names)

    def pointers(text):
        return pointer_problems("src/x.c", text, lambda p, d: SELFTEST_DOC if d == "THEORY.md" else None)

    full = ("| PYR-SAFE-01 | x | test_PYR_SAFE_01 | ✅ |\n| PYR-SAFE-03 | x | Hardware | ✅ HW |\n"
            "| FLT-LAND-01 | x | test_FLT_LAND_01 | ✅ |\n")
    cases = [
        ("a live ID passes", ids("/* [PYR-SAFE-01] */"), 0),
        ("an ID that does not exist", ids("/* PYR-SAFE-09 */"), 1),
        ("an unknown family", ids("/* BUZ-PAT-01 */"), 1),
        ("a task tag used as a requirement", ids("/* fixed in REV-12 */"), 1),
        ("a review's own document may cite its IDs",
         ids("REV-12", path="docs/code_review_2026-09-24_resolution.md", check_withdrawn=False), 0),
        ("an outside standard's ID", ids("/* UTF-8, CRC-16 */"), 0),
        ("a withdrawn ID", ids("/* PYR-SAFE-02 */"), 1),
        ("a withdrawn ID inside a range", ids("/* [PYR-SAFE-01..03] */"), 1),
        ("a withdrawn ID where it is defined", ids("PYR-SAFE-02", check_withdrawn=False), 0),
        ("an unknown DD", ids("DD-002"), 1),
        ("a complete table", rows(full), 0),
        ("a verified row with no test", rows(full.replace("test_FLT_LAND_01", "by inspection")), 1),
        ("a row naming a missing test", rows(full.replace("test_FLT_LAND_01", "test_FLT_LAND_09")), 1),
        ("a duplicate row", rows(full + "| FLT-LAND-01 | y | test_FLT_LAND_01 | ✅ |\n"), 1),
        ("a requirement with no row", rows(full.replace("| FLT-LAND-01 | x | test_FLT_LAND_01 | ✅ |\n", "")), 1),
        ("a pointer to a heading", pointers('/* See THEORY.md "The ladder" */'), 0),
        ("a pointer to no heading", pointers('/* See THEORY.md "The rungs" */'), 1),
        ("a pointer wrapped across comment lines", pointers('/* See THEORY.md\n * "The rungs" */'), 1),
        ("a heading wrapped across comment lines", pointers('/* See THEORY.md "Two\n * words here" */'), 0),
        ("several headings in one pointer", pointers('// See THEORY.md "The ladder" and "The rungs"'), 1),
        ("a pointer to no document", pointers('# See NOWHERE.md "The ladder"'), 1),
        ("a docs/ path that exists", doc_path_problems("x.c", "/* docs/a.md */", lambda d: d == "docs/a.md"), 0),
        ("a docs/ path that does not", doc_path_problems("x.c", "/* docs/b.md */", lambda d: d == "docs/a.md"), 1),
    ]
    ok = True
    for name, problems, want in cases:
        good = len(problems) == want
        ok &= good
        print(f"{'PASS' if good else 'FAIL'} {name}: {problems}")
    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
