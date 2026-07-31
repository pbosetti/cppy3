#!/usr/bin/env python3
"""Tests for migrate_v1_to_v2.py. Stdlib-only (unittest), run directly or
via ctest (see the top-level CMakeLists.txt) -- no pytest/third-party
dependency required.
"""

from __future__ import annotations

import contextlib
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import migrate_v1_to_v2 as migrate  # noqa: E402

FIXTURES_V1 = TOOLS_DIR / "fixtures" / "v1"
FIXTURES_V2_EXPECTED = TOOLS_DIR / "fixtures" / "v2_expected"


def run_main_quiet(args: list[str]) -> int:
    """Runs migrate.main(), discarding its stdout so test output stays clean."""
    with contextlib.redirect_stdout(io.StringIO()):
        return migrate.main(args)


class TokenizerTests(unittest.TestCase):
    def test_line_comment_is_protected(self):
        text = "// PythonVM\nPythonVM x;\n"
        out = migrate.apply_rules(text)
        self.assertEqual(out, "// PythonVM\nInterpreter x;\n")

    def test_block_comment_is_protected(self):
        text = "/* uses PythonVM internally */\nPythonVM x;\n"
        out = migrate.apply_rules(text)
        self.assertEqual(out, "/* uses PythonVM internally */\nInterpreter x;\n")

    def test_string_literal_is_not_split_into_a_separate_span(self):
        # A rule (get_var) needs to match text that spans across a quoted
        # argument; if the tokenizer split strings into their own excluded
        # span, this could never match. This is a regression test for
        # exactly that bug.
        text = 'cppy3::Main().getVar<long>("a", out);'
        out = migrate.apply_rules(text)
        self.assertEqual(out, "out = " + migrate.NAMESPACE_PLACEHOLDER + '.get<long>("a");')

    def test_url_like_text_in_a_string_is_not_mistaken_for_a_comment(self):
        text = 'auto s = "see http://example.com/PythonVM for details";\nPythonVM x;\n'
        out = migrate.apply_rules(text)
        # the URL inside the string must survive untouched, including its "//"
        self.assertIn('"see http://example.com/PythonVM for details"', out)
        self.assertIn("Interpreter x;", out)

    def test_raw_string_literal_is_scanned_correctly(self):
        text = 'auto s = R"(PythonVM stays here)";\nPythonVM x;\n'
        out = migrate.apply_rules(text)
        self.assertIn('R"(PythonVM stays here)"', out)
        self.assertIn("Interpreter x;", out)


class AutoRuleTests(unittest.TestCase):
    def _migrate(self, text: str) -> str:
        return migrate.apply_rules(text)

    def test_type_renames(self):
        self.assertIn("Interpreter", self._migrate("PythonVM vm;"))
        self.assertIn("Error", self._migrate("catch (const PythonException &e)"))
        self.assertIn("GilLock", self._migrate("GILLocker lock;"))
        self.assertIn("GilLock", self._migrate("ScopedGILLock lock;"))
        self.assertIn("GilRelease", self._migrate("ScopedGILRelease r;"))

    def test_gil_locker_islocked_before_generic_rename(self):
        # must become gil_held(), not GilLock::isLocked()
        out = self._migrate("if (cppy3::GILLocker::isLocked()) {}")
        self.assertIn("cppy3::gil_held()", out)
        self.assertNotIn("GilLock::isLocked", out)

    def test_method_renames(self):
        self.assertEqual(self._migrate("x.toUTF8String()"), "x.str()")
        self.assertEqual(self._migrate("x.toLong()"), "x.to<long>()")
        self.assertEqual(self._migrate("x.toDouble()"), "x.to<double>()")
        self.assertEqual(self._migrate("x.typeName()"), "x.type_name()")

    def test_exception_info_renames(self):
        self.assertEqual(self._migrate("e.info.reason"), "e.message()")
        self.assertEqual(self._migrate("e.info.type"), "e.type_name()")
        self.assertEqual(self._migrate("e.info.trace"), "e.traceback()")

    def test_var_from_and_new_ref(self):
        self.assertEqual(self._migrate("Var::from(x)"), "Var::steal(x)")
        self.assertEqual(self._migrate("result.newRef(x)"), "result = Var::steal(x)")


class AppliedFixtureTests(unittest.TestCase):
    """Runs the real CLI (via migrate.main()) against tools/fixtures/v1/*
    and checks it matches tools/fixtures/v2_expected/* exactly, and that
    manual-review findings report the right id/line."""

    def _run_apply_on_copy(self, fixture: Path) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / fixture.name
            shutil.copyfile(fixture, copy)
            rc = run_main_quiet(["--apply", str(copy)])
            self.assertEqual(rc, 0)
            return copy.read_text(encoding="utf-8")

    def test_basic_fixture_matches_expected_v2_output(self):
        migrated = self._run_apply_on_copy(FIXTURES_V1 / "basic.cpp")
        expected = (FIXTURES_V2_EXPECTED / "basic.cpp").read_text(encoding="utf-8")
        self.assertEqual(migrated, expected)

    def test_applying_twice_is_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "basic.cpp"
            shutil.copyfile(FIXTURES_V1 / "basic.cpp", copy)
            run_main_quiet(["--apply", str(copy)])
            once = copy.read_text(encoding="utf-8")
            run_main_quiet(["--apply", str(copy)])
            twice = copy.read_text(encoding="utf-8")
            self.assertEqual(once, twice)

    def test_manual_fixture_reports_expected_findings(self):
        result = migrate.process_file(FIXTURES_V1 / "manual.cpp", apply_changes=False)
        self.assertFalse(result["changed"])
        found_ids = {f["id"] for f in result["manual_review"]}
        expected_ids = {
            "main_bare",
            "get_main_module",
            "lookup_object",
            "call_free_function",
            "arguments_type",
            "create_class_instance",
            "set_argv",
            "ndarray_wrap_dim",
            "assert_cppy3",
        }
        self.assertEqual(found_ids, expected_ids)
        # lookup_object fires once each for lookupObject and lookupCallable
        lookup_lines = [f["line"] for f in result["manual_review"] if f["id"] == "lookup_object"]
        self.assertEqual(len(lookup_lines), 2)
        # ndarray_wrap_dim fires for wrap()/dim1()/dim2() each on their own line
        wrap_dim_lines = [f["line"] for f in result["manual_review"] if f["id"] == "ndarray_wrap_dim"]
        self.assertEqual(len(wrap_dim_lines), 3)

    def test_manual_bare_does_not_re_flag_lines_the_assisted_rules_already_fixed(self):
        # cppy3::Main().injectVar<...>(...) is rewritten by the "inject_var"
        # assisted rule; main_bare must not also flag it as unhandled.
        text = 'cppy3::Main().injectVar<int>("a", 2);'
        result_ids = {f["id"] for f in migrate.find_manual_matches(text)}
        self.assertNotIn("main_bare", result_ids)


class JsonOutputTests(unittest.TestCase):
    def test_json_rules_table_covers_every_rule(self):
        import json

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            migrate.main(["--json", str(FIXTURES_V1 / "manual.cpp")])
        payload = json.loads(buf.getvalue())
        rule_ids = {r["id"] for r in payload["rules"]}
        for rule in migrate.RULES:
            self.assertIn(rule["id"], rule_ids)
        for rule in migrate.MANUAL_PATTERNS:
            self.assertIn(rule["id"], rule_ids)


if __name__ == "__main__":
    unittest.main()
