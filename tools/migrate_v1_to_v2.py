#!/usr/bin/env python3
"""Migration assistant: cppy3 v1 -> v2.

Scans C++ source for v1 cppy3 API usage and either prints a unified diff of
the mechanical rewrites it can make (default), applies them in place
(--apply), or emits structured findings for editor/CI integration (--json).

This is the executable counterpart to MIGRATION.md at the repo root -- the
RULES and MANUAL_PATTERNS tables below are that document's actual data
source (--json dumps them verbatim), so the two cannot drift apart.

Three rule classes:

  auto      Unambiguous, context-free token/method renames. Applied without
            comment beyond the note attached to each rule.
  assisted  A mechanical rewrite that changes behavior, or that needs a
            cppy3::Namespace receiver the script cannot know the name of --
            those are rewritten with the placeholder receiver
            "/*YOUR_NAMESPACE*/", which will not silently compile: grep for
            it after running with --apply and replace each occurrence with
            your actual Namespace (e.g. interpreter.main()).
  manual    Flagged, never rewritten -- the transformation is either not
            expressible as a token substitution (it needs real
            restructuring) or is ambiguous without type information (e.g.
            ".reset(" is also a common std::unique_ptr/std::optional
            method name unrelated to cppy3::Var).

All rules are applied only to "code" regions of the source: line comments,
block comments, and string/char literals (including raw strings) are left
untouched by construction, not merely by convention -- see strip_non_code().

Usage:
    migrate_v1_to_v2.py [PATH ...] [--apply] [--json] [--include-ext .cpp,.hpp]

    (default)  print a unified diff of the automatic/assisted rewrites,
               plus a manual-review report, for every matched file
    --apply    write the rewrites in place instead of printing a diff
    --json     machine-readable findings instead of the diff/report
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

DEFAULT_EXTENSIONS = (".cpp", ".hpp", ".cc", ".h", ".cxx", ".hxx")
NAMESPACE_PLACEHOLDER = "/*YOUR_NAMESPACE*/"

# --- rule tables -------------------------------------------------------

RULES = [
    dict(
        id="gil_locker_islocked",
        kind="auto",
        pattern=re.compile(r"\bGILLocker::isLocked\s*\(\s*\)"),
        repl="gil_held()",
        note="GILLocker::isLocked() -> the free function gil_held().",
    ),
    dict(
        id="gil_locker_type",
        kind="auto",
        pattern=re.compile(r"\bGILLocker\b"),
        repl="GilLock",
        note="GILLocker and ScopedGILLock merge into one GilLock type.",
    ),
    dict(
        id="scoped_gil_lock_type",
        kind="auto",
        pattern=re.compile(r"\bScopedGILLock\b"),
        repl="GilLock",
        note="GILLocker and ScopedGILLock merge into one GilLock type.",
    ),
    dict(
        id="scoped_gil_release_type",
        kind="auto",
        pattern=re.compile(r"\bScopedGILRelease\b"),
        repl="GilRelease",
        note="ScopedGILRelease -> GilRelease (now a no-op if the GIL isn't held).",
    ),
    dict(
        id="python_vm_type",
        kind="auto",
        pattern=re.compile(r"\bPythonVM\b"),
        repl="Interpreter",
        note="PythonVM -> Interpreter (PyConfig-based lifecycle).",
    ),
    dict(
        id="python_exception_type",
        kind="auto",
        pattern=re.compile(r"\bPythonException\b"),
        repl="Error",
        note="PythonException -> Error (UTF-8, with a __cause__/__context__ chain).",
    ),
    dict(
        id="to_utf8_string",
        kind="auto",
        pattern=re.compile(r"\.toUTF8String\s*\(\s*\)"),
        repl=".str()",
        note=".toUTF8String() -> .str() (Var::str() is UTF-8 natively).",
    ),
    dict(
        id="to_long",
        kind="auto",
        pattern=re.compile(r"\.toLong\s*\(\s*\)"),
        repl=".to<long>()",
        note=".toLong() -> .to<long>() (Converter<T>-based extraction).",
    ),
    dict(
        id="to_double",
        kind="auto",
        pattern=re.compile(r"\.toDouble\s*\(\s*\)"),
        repl=".to<double>()",
        note=".toDouble() -> .to<double>() -- also now accepts a Python int, not just a float.",
    ),
    dict(
        id="type_name",
        kind="auto",
        pattern=re.compile(r"\.typeName\s*\(\s*\)"),
        repl=".type_name()",
        note=".typeName() -> .type_name().",
    ),
    dict(
        id="info_reason",
        kind="auto",
        pattern=re.compile(r"\.info\.reason\b"),
        repl=".message()",
        note="PythonException::info.reason -> Error::message().",
    ),
    dict(
        id="info_type",
        kind="auto",
        pattern=re.compile(r"\.info\.type\b"),
        repl=".type_name()",
        note="PythonException::info.type -> Error::type_name() (a bare name, not \"<class 'X'>\").",
    ),
    dict(
        id="info_trace",
        kind="auto",
        pattern=re.compile(r"\.info\.trace\b"),
        repl=".traceback()",
        note="PythonException::info.trace (a vector<wstring>) -> Error::traceback() (one string).",
    ),
    dict(
        id="var_from",
        kind="auto",
        pattern=re.compile(r"\bVar::from\s*\("),
        repl="Var::steal(",
        note="Var::from(x) -> Var::steal(x).",
    ),
    dict(
        id="new_ref",
        kind="auto",
        pattern=re.compile(r"\b([A-Za-z_]\w*)\.newRef\s*\("),
        repl=r"\1 = Var::steal(",
        note=".newRef(x) was a method on a live Var; Var::steal(x) is a static factory, so the call becomes an assignment.",
    ),
    dict(
        id="exec_free_function",
        kind="assisted",
        pattern=re.compile(r"\bcppy3::exec\s*\("),
        repl=NAMESPACE_PLACEHOLDER + ".exec(",
        note="Free cppy3::exec() ran in the shared __main__ dict; replace "
        + NAMESPACE_PLACEHOLDER
        + " with your cppy3::Namespace (e.g. interpreter.main()).",
    ),
    dict(
        id="eval_free_function",
        kind="assisted",
        pattern=re.compile(r"\bcppy3::eval\s*\("),
        repl=NAMESPACE_PLACEHOLDER + ".eval(",
        note="Free cppy3::eval() ran in the shared __main__ dict; replace "
        + NAMESPACE_PLACEHOLDER
        + " with your cppy3::Namespace.",
    ),
    dict(
        id="exec_script_file",
        kind="assisted",
        pattern=re.compile(r"\bcppy3::execScriptFile\s*\("),
        repl=NAMESPACE_PLACEHOLDER + ".exec_file(",
        note="execScriptFile() -> Namespace::exec_file() (tracebacks/__file__ now show the real path); replace "
        + NAMESPACE_PLACEHOLDER
        + " with your cppy3::Namespace.",
    ),
    dict(
        id="inject_var",
        kind="assisted",
        pattern=re.compile(r"\bcppy3::Main\(\)\.injectVar\s*<[^>]*>\s*\("),
        repl=NAMESPACE_PLACEHOLDER + ".set(",
        note="Main().injectVar<T>(name, value) -> ns.set(name, value); replace "
        + NAMESPACE_PLACEHOLDER
        + " with your cppy3::Namespace.",
    ),
    dict(
        id="inject",
        kind="assisted",
        pattern=re.compile(r"\bcppy3::Main\(\)\.inject\s*\("),
        repl=NAMESPACE_PLACEHOLDER + ".set(",
        note="Main().inject(name, value) -> ns.set(name, value); replace "
        + NAMESPACE_PLACEHOLDER
        + " with your cppy3::Namespace.",
    ),
    dict(
        id="get_var",
        kind="assisted",
        pattern=re.compile(
            r'\bcppy3::Main\(\)\.getVar\s*<([^>]*)>\s*\(\s*("(?:[^"\\]|\\.)*")\s*,\s*([A-Za-z_]\w*)\s*\)'
        ),
        repl=r"\3 = " + NAMESPACE_PLACEHOLDER + r".get<\1>(\2)",
        note="Main().getVar<T>(name, out) -> out = ns.get<T>(name) (a return value, not an out-parameter); replace "
        + NAMESPACE_PLACEHOLDER
        + " with your cppy3::Namespace.",
    ),
    dict(
        id="get_main_dict",
        kind="assisted",
        pattern=re.compile(r"\bcppy3::getMainDict\s*\(\s*\)"),
        repl=NAMESPACE_PLACEHOLDER + ".dict().get()",
        note="getMainDict() -> ns.dict().get(); replace " + NAMESPACE_PLACEHOLDER + " with your cppy3::Namespace.",
    ),
    dict(
        id="reset_to_borrow",
        kind="assisted",
        pattern=re.compile(r"\b([A-Za-z_]\w*)\.reset\s*\("),
        repl=r"\1 = Var::borrow(",
        note="Var::reset(x) -> Var::borrow(x) (an assignment, not a mutator). CAUTION: .reset( is also a "
        "common std::unique_ptr/std::optional method name; verify the receiver is actually a cppy3::Var "
        "before keeping this rewrite.",
    ),
]

MANUAL_PATTERNS = [
    dict(
        id="main_bare",
        # Negative lookahead excludes the .injectVar</.inject(/.getVar< call
        # patterns the assisted rules above already rewrite, so this only
        # flags *other*, unhandled cppy3::Main() uses instead of redundantly
        # re-flagging lines that were already fixed.
        pattern=re.compile(r"\bcppy3::Main\(\)(?!\s*\.\s*(?:injectVar\s*<|inject\s*\(|getVar\s*<))"),
        note="cppy3::Main() has no context-free replacement (it needs your cppy3::Namespace, e.g. "
        "interpreter.main()); the injectVar/inject/getVar-specific rules above handle the common call "
        "patterns automatically -- this flags any other, unhandled use.",
    ),
    dict(
        id="get_main_module",
        pattern=re.compile(r"\bcppy3::getMainModule\s*\(\s*\)"),
        note="getMainModule() returned the __main__ module object itself; Namespace only wraps its dict. "
        'Use PyImport_AddModule("__main__") directly if you need the module object.',
    ),
    dict(
        id="lookup_object",
        pattern=re.compile(r"\bcppy3::lookup(?:Object|Callable)\s*\("),
        note="lookupObject/lookupCallable have no direct replacement -- use Var::attr()/operator[] directly, "
        'e.g. lookupObject(m, L"a.b") becomes Var(m).attr("a").attr("b").',
    ),
    dict(
        id="call_free_function",
        pattern=re.compile(r"\bcppy3::call\s*\("),
        note="call(f, args) -> f(args...) (Var::operator()), which returns an owning Var. If you manually "
        "Py_DECREF'd the raw PyObject* call() returned, delete that decref.",
    ),
    dict(
        id="arguments_type",
        pattern=re.compile(r"\bcppy3::arguments\b"),
        note="cppy3::arguments (std::vector<Var>) was call()'s argument-list type; Var::operator() takes a "
        "variadic argument pack directly instead.",
    ),
    dict(
        id="create_class_instance",
        pattern=re.compile(r"\bcppy3::createClassInstance\s*\("),
        note='createClassInstance(name) -> ns.dict()["ClassName"](...) (an ordinary Var call).',
    ),
    dict(
        id="set_argv",
        pattern=re.compile(r"\bcppy3::setArgv\s*\("),
        note="setArgv() is removed outright (it never worked -- it dereferenced a null PyConfig*). Use "
        "Config::argv when constructing the Interpreter.",
    ),
    dict(
        id="ndarray_wrap_dim",
        pattern=re.compile(r"\.(?:wrap|dim1|dim2)\s*\("),
        note="If this is a cppy3::NDArray, note wrap()/dim1()/dim2() changed: wrap() is now a static factory "
        "(NDArray<T>::wrap(...)), and dim1()/dim2() are 0/1-indexed (v1's were off by one). Not "
        "necessarily cppy3-related if this is an unrelated type's method of the same name.",
    ),
    dict(
        id="assert_cppy3",
        pattern=re.compile(r"\bassert\s*\([^;]*\bcppy3::"),
        note="v1 used assert() (compiled out under NDEBUG) to guard invalid input; v2 throws cppy3::Error "
        "instead. Review whether this assert should become a try/catch, or can simply be removed.",
    ),
]

# --- comment/string-aware scanning --------------------------------------

_RAW_STRING_PREFIXES = ("u8R", "uR", "UR", "LR", "R")


def _raw_string_prefix_len(text: str, quote_index: int) -> int:
    """Returns the length of a raw-string prefix (u8R/uR/UR/LR/R) immediately
    preceding text[quote_index] == '"', or 0 if there isn't one."""
    for prefix in _RAW_STRING_PREFIXES:
        start = quote_index - len(prefix)
        if start < 0 or text[start:quote_index] != prefix:
            continue
        before = text[start - 1] if start > 0 else ""
        if before.isalnum() or before == "_":
            continue  # prefix is actually the tail of a longer identifier
        return len(prefix)
    return 0


def find_literal_regions(text: str) -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
    """Scans text once and returns (comment_spans, string_spans), each a
    list of (start, end) tuples.

    comment_spans (// line comments and /* block comments */) must never be
    matched by any rule -- a match overlapping one at all is rejected.

    string_spans (string/char literals, including raw strings) are handled
    differently: a match *fully contained* in one is rejected (a rule
    coincidentally matching data inside an unrelated string literal, e.g. a
    log message), but a match that legitimately *spans across* one is
    allowed -- get_var's pattern must see a call expression's text before,
    across, and after its quoted name argument as a single match, and no
    span-exclusion scheme could ever satisfy both requirements at once.
    String/char literals are still scanned over explicitly here (rather
    than ignored), so that a "//" or "/*" inside one -- e.g. a URL -- is
    not mistaken for the start of a comment.
    """
    comment_spans: list[tuple[int, int]] = []
    string_spans: list[tuple[int, int]] = []
    i, n = 0, len(text)

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j == -1 else j
            comment_spans.append((i, j))
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            comment_spans.append((i, j))
            i = j
            continue
        if c == '"':
            raw_len = _raw_string_prefix_len(text, i)
            start = i - raw_len
            if raw_len:
                open_paren = text.find("(", i + 1)
                if open_paren == -1:
                    j = n
                else:
                    delim = text[i + 1 : open_paren]
                    end_marker = ")" + delim + '"'
                    end_idx = text.find(end_marker, open_paren + 1)
                    j = n if end_idx == -1 else end_idx + len(end_marker)
            else:
                j = i + 1
                while j < n:
                    if text[j] == "\\":
                        j += 2
                        continue
                    if text[j] == '"':
                        j += 1
                        break
                    j += 1
            string_spans.append((start, j))
            i = j
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    j += 1
                    break
                j += 1
            string_spans.append((i, j))
            i = j
            continue
        i += 1
    return comment_spans, string_spans


def _overlaps_any(start: int, end: int, spans: list[tuple[int, int]]) -> bool:
    return any(s < end and start < e for s, e in spans)


def _fully_inside_any(start: int, end: int, spans: list[tuple[int, int]]) -> bool:
    return any(s <= start and end <= e for s, e in spans)


# --- rule application ---------------------------------------------------


def apply_rules(text: str) -> str:
    # Spans are recomputed before each rule, not once up front: an earlier
    # rule's substitution can change the text's length, which would shift
    # every later offset out from under a stale span list.
    for rule in RULES:
        comment_spans, string_spans = find_literal_regions(text)

        def repl(m: re.Match, rule=rule, comment_spans=comment_spans, string_spans=string_spans) -> str:
            if _overlaps_any(m.start(), m.end(), comment_spans):
                return m.group(0)
            if _fully_inside_any(m.start(), m.end(), string_spans):
                return m.group(0)
            return m.expand(rule["repl"])

        text = rule["pattern"].sub(repl, text)
    return text


def find_manual_matches(text: str) -> list[dict]:
    comment_spans, string_spans = find_literal_regions(text)
    findings = []
    for rule in MANUAL_PATTERNS:
        for m in rule["pattern"].finditer(text):
            if _overlaps_any(m.start(), m.end(), comment_spans):
                continue
            if _fully_inside_any(m.start(), m.end(), string_spans):
                continue
            line = text.count("\n", 0, m.start()) + 1
            findings.append({"id": rule["id"], "line": line, "text": m.group(0), "note": rule["note"]})
    findings.sort(key=lambda f: f["line"])
    return findings


def process_file(path: Path, apply_changes: bool) -> dict:
    text = path.read_text(encoding="utf-8")
    new_text = apply_rules(text)
    manual_review = find_manual_matches(text)
    changed = new_text != text

    diff = None
    if changed:
        if apply_changes:
            path.write_text(new_text, encoding="utf-8")
        else:
            diff = "".join(
                difflib.unified_diff(
                    text.splitlines(keepends=True),
                    new_text.splitlines(keepends=True),
                    fromfile=str(path),
                    tofile=f"{path} (migrated)",
                )
            )
    return {"path": str(path), "changed": changed, "diff": diff, "manual_review": manual_review}


# --- CLI -----------------------------------------------------------------


def find_files(paths: list[str], extensions: tuple[str, ...]) -> list[Path]:
    files: list[Path] = []
    for raw in paths:
        p = Path(raw)
        if p.is_file():
            files.append(p)
        elif p.is_dir():
            for ext in extensions:
                files.extend(sorted(p.rglob(f"*{ext}")))
    return files


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Migrate cppy3 v1 API usage to v2. See MIGRATION.md for the human-readable version "
        "of the same rules."
    )
    parser.add_argument("paths", nargs="*", default=["."], help="files or directories to scan")
    parser.add_argument("--apply", action="store_true", help="write rewrites in place instead of printing a diff")
    parser.add_argument("--json", action="store_true", help="emit machine-readable findings instead of text")
    parser.add_argument(
        "--include-ext",
        default=",".join(DEFAULT_EXTENSIONS),
        help=f"comma-separated extensions to scan within directories (default: {','.join(DEFAULT_EXTENSIONS)})",
    )
    args = parser.parse_args(argv)

    extensions = tuple(e if e.startswith(".") else f".{e}" for e in args.include_ext.split(","))
    files = find_files(args.paths, extensions)
    results = [process_file(f, args.apply) for f in files]

    if args.json:
        payload = {
            "rules": [{"id": r["id"], "kind": r["kind"], "note": r["note"]} for r in RULES]
            + [{"id": r["id"], "kind": "manual", "note": r["note"]} for r in MANUAL_PATTERNS],
            "files": results,
        }
        print(json.dumps(payload, indent=2))
        return 0

    any_changes = False
    any_manual = False
    for result in results:
        if result["diff"]:
            any_changes = True
            sys.stdout.write(result["diff"])
        elif args.apply and result["changed"]:
            any_changes = True
            print(f"# migrated: {result['path']}")
        if result["manual_review"]:
            any_manual = True
            print(f"\n# manual review needed: {result['path']}")
            for finding in result["manual_review"]:
                print(f"  {result['path']}:{finding['line']}: {finding['text']!r}")
                print(f"    -> {finding['note']}")

    if not any_changes and not any_manual:
        print("No v1 cppy3 API usage found.")
    elif args.apply and not any_manual:
        print("\nAll matched patterns were rewritten automatically. Re-run your build.")
    elif any_manual:
        print(
            f"\n{NAMESPACE_PLACEHOLDER} placeholders (if any) and the manual-review items above need "
            "your attention before this will compile. See MIGRATION.md for the full rationale."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
