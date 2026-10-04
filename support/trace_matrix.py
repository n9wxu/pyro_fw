#!/usr/bin/env python3
"""Write TRACEABILITY.md from what the tests themselves cite [TST-09, CODE-02].

    support/trace_matrix.py            rewrite TRACEABILITY.md
    support/trace_matrix.py --check    exit 1 if it is out of date

A requirement is verified by:
  - a test named for it (test_FLT_APO_01_...) or citing it in its own comment
    or body ([FLT-APO-01]);
  - a suite whose header says `Verifies [...]`;
  - a Playwright file or a hardware script that cites it;
  - a rule of support/structure_check.py;
  - a line of support/trace_notes.tsv: id, status, text. A note's status, when
    it has one, overrides the computed one; its text is added to the row.
A user need or system requirement with no evidence of its own is verified
through the requirements that derive from it.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF_RE = re.compile(r"^- \*\*([A-Z]{2,6}(?:-[A-Z]{2,7})?-\d{1,3})\*\*:\s*(.*)$")
ID_RE = re.compile(r"\b([A-Z]{2,6}(?:-[A-Z]{2,7})?)-(\d{1,3})(?:\.\.(\d{1,3}))?\b")
TEST_RE = re.compile(r"^(?:static\s+)?void\s+(test_[A-Za-z0-9_]+)\s*\(void\)", re.M)
NAMED_RE = re.compile(r"^test_([A-Z]+(?:_[A-Z]+)?)_(\d+)_")
HARDWARE_SCRIPTS = ["api_check.py", "http_stream_check.py", "bench_flight.py", "pressure_trace.py",
                    "noise_baseline.py", "test_network.py", "hr_log.py"]
MAX_NAMED = 4
OK, HW, GAP = "✅", "✅ HW", "⚠️"


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
        return f.read()


def ids_in(text, known):
    found = set()
    for fam, lo, hi in ID_RE.findall(text):
        numbers = range(int(lo), int(hi) + 1) if hi else [int(lo)]
        for n in numbers:
            rid = f"{fam}-{n:0{len(lo)}d}"
            if rid in known:
                found.add(rid)
    return found


def requirements():
    """Live requirements in document order: (section, id, text, parents)."""
    out, section = [], ""
    for line in read("REQUIREMENTS.md").splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
        m = DEF_RE.match(line.strip())
        if m and not m.group(2).startswith("Withdrawn"):
            text, _, links = m.group(2).partition("←")
            out.append((section, m.group(1), text.strip(), links))
    return out


def summary_of(text):
    text = re.sub(r"`", "", text)
    first = re.split(r"(?<=[a-z0-9)])[.:] ", text, maxsplit=1)[0].rstrip(".")
    if len(first) > 110:
        first = first[:107].rsplit(" ", 1)[0] + "..."
    return first.replace("|", "/")


def gather(known):
    tests, suites, web, hardware, structure = {}, {}, {}, {}, set()
    for path in sorted(glob.glob(os.path.join(ROOT, "test", "*.c"))):
        name = os.path.basename(path)
        text = read(os.path.join("test", name))
        header = re.search(r"Verifies \[(.*?)\]", text, re.S)
        if header:
            for rid in ids_in(header.group(1), known):
                suites.setdefault(rid, []).append(name)
        found = list(TEST_RE.finditer(text))
        for i, m in enumerate(found):
            body_end = found[i + 1].start() if i + 1 < len(found) else len(text)
            before = text[:m.start()].rstrip()
            comment = before[before.rfind("/*"):] if before.endswith("*/") else ""
            cited = ids_in(comment + text[m.start():body_end], known)
            named = NAMED_RE.match(m.group(1))
            if named:
                cited |= ids_in(named.group(1).replace("_", "-") + "-" + named.group(2), known)
            for rid in cited:
                tests.setdefault(rid, []).append((name, m.group(1)))
    for path in sorted(glob.glob(os.path.join(ROOT, "test", "web", "*.spec.js"))):
        for rid in ids_in(read(os.path.relpath(path, ROOT)), known):
            web.setdefault(rid, []).append(os.path.basename(path))
    for script in HARDWARE_SCRIPTS:
        if os.path.exists(os.path.join(ROOT, "support", script)):
            for rid in ids_in(read("support/" + script), known):
                hardware.setdefault(rid, []).append(script)
    structure = set(re.findall(r'rule\("([A-Z-]+-\d+)"', read("support/structure_check.py"))) & known
    return tests, suites, web, hardware, structure


def notes():
    out = {}
    for line in read("support/trace_notes.tsv").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        rid, status, text = (line.split("\t") + ["", ""])[:3]
        out[rid.strip()] = (status.strip(), text.strip())
    return out


def evidence_for(rid, tests, suites, web, hardware, structure):
    parts = []
    by_suite = {}
    for suite, test in tests.get(rid, []):
        by_suite.setdefault(suite, [])
        if test not in by_suite[suite]:
            by_suite[suite].append(test)
    for suite, names in by_suite.items():
        shown = ", ".join(names[:MAX_NAMED])
        more = f" and {len(names) - MAX_NAMED} more" if len(names) > MAX_NAMED else ""
        parts.append(f"{suite}: {shown}{more}")
    whole = [s for s in suites.get(rid, []) if s not in by_suite]
    if whole:
        parts.append("the suite " + ", ".join(whole))
    if rid in web:
        parts.append("Web UI: " + ", ".join(sorted(set(web[rid]))))
    if rid in structure:
        parts.append("Structure: `support/structure_check.py`")
    host = bool(parts)
    if rid in hardware:
        parts.append("Hardware: " + ", ".join(f"`support/{s}`" for s in sorted(set(hardware[rid]))))
    return parts, host, rid in hardware


def build():
    reqs = requirements()
    known = {r[1] for r in reqs}
    tests, suites, web, hardware, structure = gather(known)
    manual = notes()
    children = {}
    for _, rid, _, links in reqs:
        for parent in ids_in(links, known):
            children.setdefault(parent, []).append(rid)

    status, rows = {}, {}
    for _, rid, text, _ in reversed(reqs):  # children are defined after their parents
        parts, host, hw = evidence_for(rid, tests, suites, web, hardware, structure)
        note_status, note_text = manual.get(rid, ("", ""))
        if note_text:
            parts.append(note_text)
        computed = OK if host else HW if hw else None
        if computed is None and children.get(rid):
            kids = children[rid]
            parts.insert(0, "Through " + ", ".join(kids))
            computed = OK if all(status.get(k, GAP).startswith("✅") for k in kids) else GAP
        status[rid] = note_status or computed or GAP
        rows[rid] = "; ".join(parts) if parts else "—"

    lines = ["# Requirements Traceability Matrix", "",
             "Each live requirement of `REQUIREMENTS.md` and what verifies it. Withdrawn",
             "requirements (its Appendix A) have no row.", "",
             "This file is written by `support/trace_matrix.py` from the tests' own",
             "citations, the rules of `support/structure_check.py`, and the notes in",
             "`support/trace_notes.tsv`. Edit those, not this.", "",
             "- **✅** a host test, a web test or a structural check verifies it.",
             "- **✅ HW** verified on hardware only: a bench script or a measurement.",
             "- **⚠️** not verified, or not wholly: the row says what is owed.", ""]
    section = None
    counts = {OK: 0, HW: 0, GAP: 0, "❌": 0}
    for sec, rid, text, _ in reqs:
        if sec != section:
            section = sec
            lines += ["## " + sec, "", "| Req | Description | Verified By | Status |",
                      "|-----|-------------|-------------|--------|"]
        s = status[rid]
        key = "❌" if s.startswith("❌") else GAP if s.startswith("⚠") else HW if "HW" in s else OK
        counts[key] += 1
        lines.append(f"| {rid} | {summary_of(text)} | {rows[rid]} | {s} |")
        nxt = reqs[reqs.index((sec, rid, text, _)) + 1][0] if reqs.index((sec, rid, text, _)) + 1 < len(reqs) else None
        if nxt != sec:
            lines.append("")
    lines += ["---", "", "## Summary", "", "| Status | Count |", "|--------|-------|",
              f"| ✅ Verified by a host test, a web test or a structural check | {counts[OK]} |",
              f"| ⚠️ Not verified, or not wholly | {counts[GAP]} |",
              f"| ❌ Not implemented | {counts['❌']} |",
              f"| ✅ HW (verified on hardware only) | {counts[HW]} |", "",
              "`support/trace_check.py --counts` computes these, and CI fails when this table",
              "disagrees or when `support/trace_matrix.py --check` finds the file out of date.", ""]
    return "\n".join(lines), counts


def main():
    text, counts = build()
    path = os.path.join(ROOT, "TRACEABILITY.md")
    if "--check" in sys.argv:
        if read("TRACEABILITY.md") != text:
            print("TRACEABILITY.md is out of date: run support/trace_matrix.py")
            return 1
        print("trace_matrix: TRACEABILITY.md is up to date")
        return 0
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("trace_matrix:", ", ".join(f"{k} {v}" for k, v in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
