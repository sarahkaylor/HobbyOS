#!/usr/bin/env python3
"""Tests for tools/cstyle.py — the HobbyOS C style formatter.

Run:  python3 tools/test_cstyle.py          (or: make style-test)

Covers: tokenizer, tab handling, indentation, K&R brace joining, control
structures, continuations, comments, preprocessor handling, string/char
literal protection, CLI behavior, idempotency, and the token-preservation
safety guarantee — including a corpus sweep over the real src/ tree.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import cstyle  # noqa: E402


def fmt(src, **kw):
    return cstyle.format_source(src, **kw)


class TestTokenizer(unittest.TestCase):
    def test_token_kinds(self):
        toks = cstyle.tokenize('int x = "s"; /* c */ // line\n#define A 1\n')
        kinds = [t.kind for t in toks]
        self.assertIn("code", kinds)
        self.assertIn("string", kinds)
        self.assertIn("bcomment", kinds)
        self.assertIn("icomment", kinds)
        self.assertIn("preproc", kinds)

    def test_string_with_escapes(self):
        toks = cstyle.tokenize(r'char *s = "a\"b\\c";')
        strings = [t for t in toks if t.kind == "string"]
        self.assertEqual(strings[0].text, r'"a\"b\\c"')

    def test_char_literal_with_quote(self):
        toks = cstyle.tokenize(r"char c = '\'';")
        chars = [t for t in toks if t.kind == "char"]
        self.assertEqual(len(chars), 1)
        self.assertEqual(chars[0].text, r"'\''")

    def test_multi_char_operators(self):
        toks = [t.text for t in cstyle.tokenize("a>>=1; b<=2; c->d; e&&f;")]
        self.assertIn(">>=", toks)
        self.assertIn("<=", toks)
        self.assertIn("->", toks)
        self.assertIn("&&", toks)

    def test_line_attribution(self):
        toks = cstyle.tokenize("int a;\nint b;\n")
        a = [t for t in toks if t.text == "a"][0]
        b = [t for t in toks if t.text == "b"][0]
        self.assertEqual(a.start_line, 1)
        self.assertEqual(b.start_line, 2)

    def test_spliced_string_single_token(self):
        toks = cstyle.tokenize('"a\\\nb"\n')
        strings = [t for t in toks if t.kind == "string"]
        self.assertEqual(len(strings), 1)
        self.assertEqual(strings[0].start_line, 1)
        self.assertEqual(strings[0].end_line, 2)

    def test_comments_not_inside_strings(self):
        toks = cstyle.tokenize('puts("/* not a comment */");')
        self.assertEqual([t.kind for t in toks if "comment" in t.kind], [])


class TestTabs(unittest.TestCase):
    def test_leading_tab_expanded(self):
        self.assertEqual(fmt("\tint x;\n"), "int x;\n")

    def test_nested_tab_indent(self):
        src = "void f(void) {\n\tif (x) {\n\t\tg();\n\t}\n}\n"
        want = "void f(void) {\n  if (x) {\n    g();\n  }\n}\n"
        self.assertEqual(fmt(src), want)

    def test_tab_inside_string_preserved(self):
        src = 'x = "a\tb";\n'
        out = fmt(src)
        self.assertIn("\t", out)
        self.assertEqual(out, 'x = "a\tb";\n')

    def test_tab_inside_char_preserved(self):
        src = "c = '\\t';\n"
        self.assertEqual(fmt(src), src)

    def test_midline_tab_expanded_to_stop(self):
        # 1 space + tab -> tab stop at 8; then reindent applies (level 0)
        self.assertEqual(fmt("ab\tcd;\n"), "ab      cd;\n")

    def test_tab_in_continuation_visual_alignment(self):
        src = "void g(void) {\n\tfoo(a,\n\t    b);\n}\n"
        want = "void g(void) {\n  foo(a,\n      b);\n}\n"
        self.assertEqual(fmt(src), want)


class TestIndentation(unittest.TestCase):
    def test_four_space_reindent(self):
        src = "void f(void) {\n    if (x) {\n        g();\n    }\n}\n"
        want = "void f(void) {\n  if (x) {\n    g();\n  }\n}\n"
        self.assertEqual(fmt(src), want)

    def test_unbraced_if(self):
        src = "void f(void) {\n    if (x)\n        return;\n}\n"
        want = "void f(void) {\n  if (x)\n    return;\n}\n"
        self.assertEqual(fmt(src), want)

    def test_unbraced_else(self):
        src = "void f(void) {\nif (x)\ny();\nelse\nz();\n}\n"
        want = "void f(void) {\n  if (x)\n    y();\n  else\n    z();\n}\n"
        self.assertEqual(fmt(src), want)

    def test_dangling_else_binds_correctly(self):
        src = ("void f(void) {\n"
               "  if (a)\n"
               "    if (b)\n"
               "      x();\n"
               "    else\n"
               "      y();\n"
               "}\n")
        self.assertEqual(fmt(src), src)

    def test_nested_closure(self):
        src = "void f(void) {\nif (x)\nwhile (y)\nz();\n}\n"
        want = ("void f(void) {\n"
                "  if (x)\n"
                "    while (y)\n"
                "      z();\n"
                "}\n")
        self.assertEqual(fmt(src), want)

    def test_deep_closes(self):
        src = "void f(void) {\nif (x) {\nif (y) {\nz();\n}\n}\n}\n"
        want = ("void f(void) {\n"
                "  if (x) {\n"
                "    if (y) {\n"
                "      z();\n"
                "    }\n"
                "  }\n"
                "}\n")
        self.assertEqual(fmt(src), want)


class TestBraceJoining(unittest.TestCase):
    def test_allman_if(self):
        self.assertEqual(fmt("if (x)\n{\ny();\n}\n"),
                         "if (x) {\n  y();\n}\n")

    def test_allman_else(self):
        src = "if (a)\n{\nb();\n}\nelse\n{\nc();\n}\n"
        want = "if (a) {\n  b();\n} else {\n  c();\n}\n"
        self.assertEqual(fmt(src), want)

    def test_else_if_chain(self):
        src = ("if (a)\n{\nx();\n}\nelse if (b)\n{\ny();\n}\n")
        want = "if (a) {\n  x();\n} else if (b) {\n  y();\n}\n"
        self.assertEqual(fmt(src), want)

    def test_allman_function(self):
        src = "static int\nparse (const char *s)\n{\nreturn 0;\n}\n"
        want = "static int parse (const char *s) {\n  return 0;\n}\n"
        self.assertEqual(fmt(src), want)

    def test_allman_function_one_line_header(self):
        self.assertEqual(fmt("void f(void)\n{\nx();\n}\n"),
                         "void f(void) {\n  x();\n}\n")

    def test_do_while(self):
        src = "do\n{\nx();\n}\nwhile (a);\n"
        want = "do {\n  x();\n} while (a);\n"
        self.assertEqual(fmt(src), want)

    def test_do_while_unbraced(self):
        src = "do\nx();\nwhile (a);\n"
        want = "do\n  x();\nwhile (a);\n"
        self.assertEqual(fmt(src), want)

    def test_switch(self):
        src = ("switch (x)\n{\ncase 1:\ncase 2:\na();\nbreak;\n"
               "default:\nb();\n}\n")
        want = ("switch (x) {\ncase 1:\ncase 2:\n  a();\n  break;\n"
                "default:\n  b();\n}\n")
        self.assertEqual(fmt(src), want)

    def test_struct_definition(self):
        src = "struct point\n{\nint x;\nint y;\n};\n"
        want = "struct point {\n  int x;\n  int y;\n};\n"
        self.assertEqual(fmt(src), want)

    def test_typedef_struct(self):
        src = "typedef struct\n{\nint x;\n} point_t;\n"
        want = "typedef struct {\n  int x;\n} point_t;\n"
        self.assertEqual(fmt(src), want)

    def test_enum(self):
        src = "enum mode\n{\nM_A,\nM_B,\n};\n"
        want = "enum mode {\n  M_A,\n  M_B,\n};\n"
        self.assertEqual(fmt(src), want)

    def test_initializer_list_jam(self):
        src = "struct point p =\n{\n0, 0\n};\n"
        want = "struct point p = {\n  0, 0\n};\n"
        self.assertEqual(fmt(src), want)

    def test_array_initializer(self):
        src = "int xs[] =\n{\n1,\n2,\n};\n"
        want = "int xs[] = {\n  1,\n  2,\n};\n"
        self.assertEqual(fmt(src), want)

    def test_brace_inside_string_not_jammed(self):
        src = 'char *s = "\\n\\\n";\nif (x)\n{\ny();\n}\n'
        out = fmt(src)
        self.assertIn("if (x) {", out)

    def test_close_brace_with_trailing_comment(self):
        src = "if (x)\n{\ny();\n}\n/* after */\nz();\n"
        want = "if (x) {\n  y();\n}\n/* after */\nz();\n"
        self.assertEqual(fmt(src), want)


class TestContinuations(unittest.TestCase):
    def test_add_argument_alignment(self):
        src = "void f(void) {\n    int x = add(a,\n                b);\n}\n"
        want = "void f(void) {\n  int x = add(a,\n              b);\n}\n"
        self.assertEqual(fmt(src), want)

    def test_operator_continuation(self):
        src = ("void f(void) {\n"
               "    if (a &&\n"
               "        b)\n"
               "        g();\n"
               "}\n")
        want = ("void f(void) {\n"
                "  if (a &&\n"
                "      b)\n"
                "    g();\n"
                "}\n")
        self.assertEqual(fmt(src), want)

    def test_comma_continuation(self):
        src = "foo(a,\n    b);\n"
        self.assertEqual(fmt(src), "foo(a,\n    b);\n")

    def test_implicit_string_concat(self):
        src = ('const char *w =\n'
               '    "one\\n"\n'
               '    "two\\n";\n')
        self.assertEqual(fmt(src), src)

    def test_string_only_lines_in_call(self):
        src = ("void t(void) {\n"
               "    check(strcmp(row,\n"
               "                 \"00000000  41 42\",\n"
               "                 \"  |AB|\") == 0,\n"
               "          \"msg\");\n"
               "}\n")
        want = ("void t(void) {\n"
                "  check(strcmp(row,\n"
                "               \"00000000  41 42\",\n"
                "               \"  |AB|\") == 0,\n"
                "        \"msg\");\n"
                "}\n")
        self.assertEqual(fmt(src), want)

    def test_ternary_continuation(self):
        src = ("void t2(void) {\n"
               "    ent->size = have_size ? real_size\n"
               + " " * 26 + ": fallback_size;\n"
               "}\n")
        want = ("void t2(void) {\n"
                "  ent->size = have_size ? real_size\n"
                + " " * 24 + ": fallback_size;\n"
                "}\n")
        self.assertEqual(fmt(src), want)

    def test_logical_or_line_start(self):
        src = ("if (a\n"
               "    || b)\n"
               "    g();\n")
        want = ("if (a\n"
                "    || b)\n"
                "  g();\n")
        self.assertEqual(fmt(src), want)

    def test_close_brace_line_not_continuation(self):
        src = ("static const char *files[] = {\n"
               "    \"A\",\n"
               "    \"B\"\n"
               "};\n")
        want = ("static const char *files[] = {\n"
                "  \"A\",\n"
                "  \"B\"\n"
                "};\n")
        self.assertEqual(fmt(src), want)

    def test_trailing_comma_element_lines(self):
        src = ("int xs[] = {\n"
               "    1,\n"
               "    2,\n"
               "};\n")
        self.assertEqual(fmt(src), "int xs[] = {\n  1,\n  2,\n};\n")


class TestComments(unittest.TestCase):
    def test_comment_sits_at_next_code_level(self):
        src = "void f(void) {\n/* setup */\ng();\n}\n"
        want = "void f(void) {\n  /* setup */\n  g();\n}\n"
        self.assertEqual(fmt(src), want)

    def test_comment_before_closing_brace_stays_inside(self):
        src = "void f(void) {\n  g();\n  /* done */\n}\n"
        self.assertEqual(fmt(src), src)

    def test_comment_block_interior_preserved(self):
        src = ("void f(void) {\n"
               "    /* first line\n"
               "       second line  \n"
               "         third */\n"
               "    g();\n"
               "}\n")
        want = ("void f(void) {\n"
                "  /* first line\n"
                "     second line\n"
                "       third */\n"
                "  g();\n"
                "}\n")
        self.assertEqual(fmt(src), want)

    def test_trailing_comment_stays(self):
        src = "int x = 1;  /* note */\n"
        self.assertEqual(fmt(src), src)

    def test_header_comment_at_column_zero(self):
        src = "/* file header */\nint x;\n"
        self.assertEqual(fmt(src), src)

    def test_line_comment_only_line(self):
        src = "void f(void) {\n// note\ng();\n}\n"
        want = "void f(void) {\n  // note\n  g();\n}\n"
        self.assertEqual(fmt(src), want)


class TestStringsAndLiterals(unittest.TestCase):
    def test_multiline_spliced_string_preserved(self):
        src = ('void f(void) {\n'
               '    printf(_("Try `%s --help\' for more information.\\n"),\n'
               '           program_name);\n'
               '}\n')
        want = ('void f(void) {\n'
                '  printf(_("Try `%s --help\' for more information.\\n"),\n'
                '         program_name);\n'
                '}\n')
        self.assertEqual(fmt(src), want)

    def test_string_continuation_line_untouched(self):
        src = ('void f(void) {\n'
               '  printf(_("Usage: %s [OPTION]... [FILE]...\\n\\\n'
               '"), program_name);\n'
               '}\n')
        # Lines starting inside a string literal keep their exact bytes.
        self.assertEqual(fmt(src), src)

    def test_braces_inside_literals_ignored(self):
        src = 'void f(void) {\n  puts("}{");\n  char c = \'}\';\n}\n'
        self.assertEqual(fmt(src), src)

    def test_percent_and_escapes_untouched(self):
        src = 'x = sprintf(b, "%d\\t%s", 1, "a");\n'
        self.assertEqual(fmt(src), src)


class TestPreprocessor(unittest.TestCase):
    def test_directive_to_column_zero(self):
        src = "    #define N 4\nint x;\n"
        self.assertEqual(fmt(src), "#define N 4\nint x;\n")

    def test_directive_inside_function(self):
        src = ("void f(void) {\n"
               "    #ifdef DEBUG\n"
               "    x();\n"
               "    #endif\n"
               "}\n")
        want = ("void f(void) {\n"
                "#ifdef DEBUG\n"
                "  x();\n"
                "#endif\n"
                "}\n")
        self.assertEqual(fmt(src), want)

    def test_macro_continuation_preserved(self):
        src = ("#define M(x) do { \\\n"
               "  x(); \\\n"
               "} while (0)\n")
        self.assertEqual(fmt(src), src)

    def test_alternate_branches_no_inflation(self):
        src = ("#if A\n"
               "static void a(void) {\n"
               "  one();\n"
               "}\n"
               "#else\n"
               "static void b(void) {\n"
               "  two();\n"
               "}\n"
               "#endif\n"
               "\n"
               "int c;\n")
        self.assertEqual(fmt(src), src)

    def test_split_function_each_branch_opens_brace(self):
        src = ("#ifdef X\n"
               "void f(int a) {\n"
               "  x();\n"
               "#else\n"
               "void f(void) {\n"
               "  y();\n"
               "#endif\n"
               "  shared();\n"
               "}\n")
        self.assertEqual(fmt(src), src)

    def test_single_branch_function_keeps_but_restores(self):
        src = ("#ifdef X\n"
               "static void helper(void) {\n"
               "  h();\n"
               "}\n"
               "#endif\n"
               "\n"
               "int g;\n")
        self.assertEqual(fmt(src), src)

    def test_elif_chain(self):
        # Top-level guarded code is normalized to its nesting depth: the
        # guard itself adds no indentation.
        src = ("#if A\n"
               "    a();\n"
               "#elif B\n"
               "  b();\n"
               "#else\n"
               "c();\n"
               "#endif\n")
        want = ("#if A\n"
                "a();\n"
                "#elif B\n"
                "b();\n"
                "#else\n"
                "c();\n"
                "#endif\n")
        self.assertEqual(fmt(src), want)

    def test_nested_conditionals(self):
        src = ("#if A\n"
               "#if B\n"
               "    x();\n"
               "#endif\n"
               "      y();\n"
               "#endif\n")
        want = ("#if A\n"
                "#if B\n"
                "x();\n"
                "#endif\n"
                "y();\n"
                "#endif\n")
        self.assertEqual(fmt(src), want)

    def test_guarded_code_inside_function_keeps_depth(self):
        src = ("void f(void) {\n"
               "  x();\n"
               "#ifdef FOO\n"
               "  y();\n"
               "#endif\n"
               "}\n")
        self.assertEqual(fmt(src), src)


class TestWhitespaceHygiene(unittest.TestCase):
    def test_trailing_whitespace_removed(self):
        self.assertEqual(fmt("int x;   \n"), "int x;\n")

    def test_blank_line_with_spaces(self):
        self.assertEqual(fmt("int x;\n   \nint y;\n"), "int x;\n\nint y;\n")

    def test_exactly_one_final_newline(self):
        self.assertEqual(fmt("int x;"), "int x;\n")
        self.assertEqual(fmt("int x;\n\n\n"), "int x;\n")

    def test_empty_input(self):
        self.assertEqual(fmt(""), "")

    def test_whitespace_only_input(self):
        self.assertEqual(fmt("   \n\t\n"), "\n")

    def test_blank_lines_between_functions_kept(self):
        src = "void a(void) {\n}\n\n\nvoid b(void) {\n}\n"
        self.assertEqual(fmt(src), src)

    def test_splice_line_keeps_backslash_last(self):
        src = "#define A \\\n  1\n"
        out = fmt(src)
        self.assertTrue(out.splitlines()[0].endswith("\\"))


class TestSafetyGuarantee(unittest.TestCase):
    def test_tokens_equal_ignores_whitespace_only(self):
        a = cstyle.tokenize("int x=1;\n")
        b = cstyle.tokenize("int  x = 1 ;\n")
        self.assertTrue(cstyle.tokens_equal(a, b))

    def test_tokens_equal_detects_string_change(self):
        a = cstyle.tokenize('x = "ab";\n')
        b = cstyle.tokenize('x = "a b";\n')
        self.assertFalse(cstyle.tokens_equal(a, b))

    def test_tokens_equal_detects_missing_token(self):
        a = cstyle.tokenize("f(a);\n")
        b = cstyle.tokenize("f();\n")
        self.assertFalse(cstyle.tokens_equal(a, b))

    def test_format_preserves_tokens_on_gnarly_input(self):
        src = ('#include <stdio.h>\n'
               '#define SQ(x) ((x)*(x))\n'
               'static const char *msg =\n'
               '    "part1 \\"q\\" "\n'
               '    "part2\\n";\n'
               'int main(void) {\n'
               '    for (int i = 0; i < 3; i++)\n'
               '        printf("%d %s\\n", SQ(i), msg);\n'
               '    return 0;\n'
               '}\n')
        out = fmt(src)
        self.assertTrue(cstyle.tokens_equal(cstyle.tokenize(src),
                                            cstyle.tokenize(out)))

    def test_format_raises_rather_than_corrupt(self):
        # Feed format_source an input whose 'output' would differ; the only
        # way it can differ is a bug — simulate by monkeypatching emit.
        src = "int x;\n"
        orig_emit = cstyle.emit_line

        def bad_emit(line):
            return orig_emit(line).replace("x", "y")

        cstyle.emit_line = bad_emit
        try:
            with self.assertRaises(AssertionError):
                fmt(src)
        finally:
            cstyle.emit_line = orig_emit
        # And the safe path still works afterwards.
        self.assertEqual(fmt(src), src)


class TestIdempotency(unittest.TestCase):
    SNIPPETS = [
        "void f(void) {\n  if (x) {\n    g();\n  } else {\n    h();\n  }\n}\n",
        "#ifdef X\nvoid f(int a) {\n  x();\n#else\nvoid f(void) {\n  y();\n#endif\n  shared();\n}\n",
        "switch (x) {\ncase 1:\n  a();\n  break;\n}\n",
        "do {\n  x();\n} while (a);\n",
        "int xs[] = {\n  1,\n  2,\n};\n",
        "a = b ? c\n      : d;\n",
        'const char *m =\n    "a"\n    "b";\n',
    ]

    def test_idempotent(self):
        for src in self.SNIPPETS:
            once = fmt(src)
            twice = fmt(once)
            self.assertEqual(once, twice, f"not idempotent: {src!r}")


class TestCLI(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="cstyle_test_")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def write(self, name, text):
        path = os.path.join(self.dir, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)
        return path

    def run_cli(self, *args):
        return subprocess.run(
            [sys.executable, os.path.join(HERE, "cstyle.py"), *args],
            capture_output=True, text=True)

    def test_check_exit_codes(self):
        bad = self.write("bad.c", "void f(void)\n{\nx();\n}\n")
        r = self.run_cli("check", self.dir)
        self.assertEqual(r.returncode, 1)
        r2 = self.run_cli("format", self.dir, "-q")
        self.assertEqual(r2.returncode, 0)
        r3 = self.run_cli("check", self.dir, "-q")
        self.assertEqual(r3.returncode, 0)
        with open(bad) as fh:
            self.assertEqual(fh.read(), "void f(void) {\n  x();\n}\n")

    def test_diff_prints_unified_diff(self):
        self.write("bad.c", "void f(void)\n{\n}\n")
        r = self.run_cli("diff", self.dir, "-q")
        self.assertEqual(r.returncode, 0)
        self.assertIn("---", r.stdout)
        self.assertIn("+++", r.stdout)
        self.assertIn("void f(void) {", r.stdout)

    def test_no_files_found(self):
        self.write("readme.txt", "hello\n")
        r = self.run_cli("check", self.dir)
        self.assertEqual(r.returncode, 2)

    def test_vendored_dirs_skipped(self):
        good = self.write("ok.c", "int x;\n")
        self.write("third_party/vendor.c", "void f(void)\n{\nx();\n}\n")
        self.write("bootloader/limine.c", "void f(void)\n{\nx();\n}\n")
        r = self.run_cli("check", self.dir, "-q")
        self.assertEqual(r.returncode, 0)
        with open(good) as fh:
            self.assertEqual(fh.read(), "int x;\n")

    def test_file_argument(self):
        bad = self.write("bad.c", "void f(void)\n{\n}\n")
        r = self.run_cli("check", bad, "-q")
        self.assertEqual(r.returncode, 1)
        # A file whose formatting already matches must pass.
        good = self.write("good.c", "int x;\n")
        r2 = self.run_cli("check", good, "-q")
        self.assertEqual(r2.returncode, 0)


class TestCorpusSweep(unittest.TestCase):
    """Format every file in the real src/ tree: tokens preserved, idempotent."""

    def test_full_src_tree(self):
        src_dir = os.path.join(REPO, "src")
        if not os.path.isdir(src_dir):
            self.skipTest("no src/ tree next to tools/")
        files = cstyle.find_files([src_dir])
        self.assertGreater(len(files), 50)
        for path in files:
            with open(path, "r", encoding="utf-8") as fh:
                src = fh.read()
            try:
                out = fmt(src)
            except AssertionError as exc:
                self.fail(f"{path}: {exc}")
            self.assertTrue(
                cstyle.tokens_equal(cstyle.tokenize(src), cstyle.tokenize(out)),
                f"{path}: token stream changed")
            self.assertEqual(fmt(out), out, f"{path}: not idempotent")

    def test_formatted_tree_is_stable(self):
        """check must pass once the tree has been formatted (runs only if
        the tree was already formatted — otherwise informational)."""
        src_dir = os.path.join(REPO, "src")
        if not os.path.isdir(src_dir):
            self.skipTest("no src/ tree next to tools/")
        files = cstyle.find_files([src_dir])
        dirty = []
        for path in files:
            with open(path, "r", encoding="utf-8") as fh:
                src = fh.read()
            if fmt(src) != src:
                dirty.append(path)
        if dirty and os.environ.get("CSTYLE_REQUIRE_FORMATTED"):
            self.fail("unformatted files:\n" + "\n".join(dirty[:20]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
