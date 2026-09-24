#!/usr/bin/env python3
"""cstyle — the HobbyOS C formatting tool.

Enforces the project C style guide (see STYLE.md):

  1. Two-space indentation, spaces never tabs (except inside string/char
     literals, where a tab is content).
  2. K&R brace placement: an opening brace goes on the same line as the
     statement that opens the block (if/for/while/else/switch/do/function
     definition/struct/enum/union/typedef/initializer list).  A closing
     brace sits on its own line, except that '}' joins 'else', 'else if'
     and the 'while' of a do-while.
  3. A function definition's return type and name stay on one line.
  4. No trailing whitespace; exactly one final newline; LF line endings.

The formatter is deliberately conservative: it only ever changes
whitespace and line structure (brace joins).  It never adds or removes
tokens, never rewrites string/char literal content, keeps backslash-spliced
macro bodies intact (tab expansion only), and shifts continuation lines so
their relative alignment survives re-indentation.

Before a file is written, the formatted text is re-tokenized and compared
token-by-token with the original; if anything other than whitespace
changed, the file is not written and an error is reported.

Usage:
    python3 tools/cstyle.py format [paths...]   # rewrite files in place
    python3 tools/cstyle.py check  [paths...]   # exit 1 if any file differs
    python3 tools/cstyle.py diff   [paths...]   # show unified diffs

Paths default to 'src'.  Directories are searched recursively for *.c and
*.h files; vendored trees (third_party/, bootloader/), the vendored gnulib
regex engine files (see EXCLUDE_FILES), and the vendored GNU sed tree
(src/user/sed/, see EXCLUDE_PREFIXES) are skipped.
"""

from __future__ import annotations

import argparse
import difflib
import os
import re
import sys

INDENT_WIDTH = 2
TAB_STOP = 8

# Directories never touched by the default tree walk (vendored upstream code
# and build output; see STYLE.md "Scope").
EXCLUDE_DIRS = {
    ".git", ".github", "third_party", "bootloader", "obj", "docs",
    "__pycache__", ".vscode",
}

DEFAULT_PATHS = ["src"]

# Individual vendored files the formatter must leave byte-exact.  The gnulib
# regex engine lives under src/libc (it is compiled into libc.a), but it is
# refreshed verbatim from gnulib, so it is out of scope exactly like
# third_party/ trees.  Keys are repo-root-relative, slash-separated paths.
EXCLUDE_FILES = {
    "src/libc/include/intprops.h",
    "src/libc/include/regex.h",
    "src/libc/include/verify.h",
    "src/libc/src/regcomp.c",
    "src/libc/src/regex_internal.c",
    "src/libc/src/regex_internal.h",
    "src/libc/src/regexec.c",
}

# Repo-relative directory prefixes kept byte-exact (upstream GNU sources and
# their generated config live under src/user/sed/; only the two hand-written
# support headers follow this guide, and they are reviewed with it in mind).
EXCLUDE_PREFIXES = {
    "src/user/sed/",
}

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _repo_rel(path):
    """Repo-root-relative, slash-normalized key for the exclusion lists."""
    rel = os.path.relpath(os.path.abspath(path), _REPO_ROOT)
    return rel.replace(os.sep, "/")

# When one of these tokens ends a line, the next line continues the same
# logical line and keeps its relative alignment (shifted with its anchor).
CONTINUATION_END_TOKENS = {
    ",", "&&", "||", "|", "&", "+", "-", "*", "/", "%", "=", "==", "!=",
    "<", ">", "<=", ">=", "?", ":", "<<", ">>", "^", "~", ".", "(", "[",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", "->",
}

# Multi-character operators, longest first (used by the tokenizer).
MULTI_CHAR_OPS = (
    "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==",
    "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##",
)

CONTROL_KEYWORDS = {"if", "for", "while", "switch"}
TYPE_HEADER_KEYWORDS = {"struct", "enum", "union", "typedef"}

ALL_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while",
}


# ---------------------------------------------------------------------------
# Tokenizer
# ---------------------------------------------------------------------------

class Token:
    """A lexical token with its source location.

    kind is one of:
      code      — C code token (identifier, number or single character)
      string    — "..." literal (may span lines via backslash-newline)
      char      — '...' literal
      icomment  — // comment (may span lines via trailing backslash)
      bcomment  — /* ... */ comment
      preproc   — a whole preprocessor logical line (directive plus any
                  backslash-spliced continuation lines)

    spans, for string/char tokens, is a list of (line_no, start_off, end_off)
    covering the literal characters on each physical line (offsets are
    character offsets within the line); used to protect literal tabs from
    tab expansion.
    """

    __slots__ = ("kind", "text", "start_line", "end_line", "spans")

    def __init__(self, kind, text, start_line, end_line, spans=None):
        self.kind = kind
        self.text = text
        self.start_line = start_line
        self.end_line = end_line
        self.spans = spans or []

    def __repr__(self):  # pragma: no cover - debugging aid
        return f"Token({self.kind!r}, {self.text!r}, {self.start_line}-{self.end_line})"


def tokenize(src, no_preproc=False):
    """Tokenize C source into a flat token list; whitespace is skipped."""
    toks = []
    n = len(src)
    line = 1
    i = 0
    line_start = 0  # offset of the current line's first character

    def splice_at(k):
        """True if the newline ending at offset k is escaped by a backslash."""
        return k > 0 and src[k - 1] == "\\"

    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            line_start = i
            continue
        if c in " \t\r":
            i += 1
            continue

        # Preprocessor directive: '#' is the first non-blank character.
        if c == "#" and not no_preproc and src[line_start:i].strip() == "":
            j = i
            while True:
                nl = src.find("\n", j)
                if nl == -1:
                    j = n
                    break
                if splice_at(nl):
                    j = nl + 1
                    continue
                j = nl
                break
            text = src[i:j]
            toks.append(Token("preproc", text, line, line + text.count("\n")))
            line += text.count("\n")
            i = j
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = i
            while True:
                nl = src.find("\n", j)
                if nl == -1:
                    j = n
                    break
                if splice_at(nl):
                    j = nl + 1
                    continue
                j = nl
                break
            text = src[i:j]
            toks.append(Token("icomment", text, line, line + text.count("\n")))
            line += text.count("\n")
            i = j
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j == -1 else j + 2
            text = src[i:j]
            toks.append(Token("bcomment", text, line, line + text.count("\n")))
            line += text.count("\n")
            i = j
            continue

        if c in "\"'":
            start = i
            start_line = line
            spans = []
            j = i + 1
            cur_line = line
            seg_start = line_start          # offset of cur_line's first char
            span_start = i - seg_start      # offset of the opening quote
            while j < n:
                ch = src[j]
                if ch == "\\" and j + 1 < n:
                    if src[j + 1] == "\n":  # backslash-newline splice
                        spans.append((cur_line, span_start, j + 1 - seg_start))
                        cur_line += 1
                        seg_start = j + 2
                        span_start = 0
                        j += 2
                        continue
                    j += 2                      # ordinary escape
                    continue
                if ch == c:                     # closing quote
                    j += 1
                    break
                if ch == "\n":                  # unterminated (tolerant)
                    break
                j += 1
            spans.append((cur_line, span_start, j - seg_start))
            text = src[start:j]
            toks.append(Token("string" if c == '"' else "char", text, start_line,
                              start_line + text.count("\n"), spans))
            if text.count("\n"):
                line = start_line + text.count("\n")
                line_start = src.rfind("\n", 0, j) + 1
            i = j
            continue

        if c.isalpha() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            toks.append(Token("code", src[i:j], line, line))
            i = j
            continue
        if c.isdigit():
            j = i
            while j < n and (src[j].isalnum() or src[j] in "._"):
                j += 1
            toks.append(Token("code", src[i:j], line, line))
            i = j
            continue

        # Multi-character operators (longest match first).
        for op in MULTI_CHAR_OPS:
            if src.startswith(op, i):
                toks.append(Token("code", op, line, line))
                i += len(op)
                break
        else:
            toks.append(Token("code", c, line, line))
            i += 1

    return toks


def normalize_tokens(tokens):
    """A whitespace-insensitive comparable form of a token stream.

    Code and literal tokens compare exactly (literal bytes are content);
    comments compare with whitespace runs collapsed; preprocessor tokens
    are re-tokenized so only their tokens matter.
    """
    out = []
    for t in tokens:
        if t.kind == "code" or t.kind in ("string", "char"):
            out.append((t.kind, t.text))
        elif t.kind == "preproc":
            out.append(("preproc", tuple(normalize_tokens(tokenize(t.text, no_preproc=True)))))
        else:  # comments
            out.append((t.kind, " ".join(t.text.split())))
    return out


def tokens_equal(a, b):
    return normalize_tokens(a) == normalize_tokens(b)


# ---------------------------------------------------------------------------
# Per-line records and classification
# ---------------------------------------------------------------------------

class Line:
    __slots__ = (
        "no", "raw", "exp", "kind", "code_idx", "level", "is_case", "is_label",
        "is_cont", "shift_from", "delta", "merged_into", "parts", "do_close",
        "untouchable", "soft_block_head", "old_width", "lead_chars",
    )

    def __init__(self, no, raw):
        self.no = no
        self.raw = raw
        self.exp = raw            # tab-expanded text
        self.kind = ""            # blank | code | comment | preproc |
                                  # preproc_cont | soft | opaque
        self.code_idx = []        # indices into the flat code token list
        self.level = None
        self.is_case = False
        self.is_label = False
        self.is_cont = False
        self.shift_from = None    # line whose delta this line follows
        self.delta = 0
        self.merged_into = None   # line this one was merged onto
        self.parts = []           # lines merged onto this one
        self.do_close = False     # this line's '}' closes a do{} block
        self.untouchable = False  # line begins inside a literal: keep raw bytes
        self.soft_block_head = None  # comment-block head for interior lines
        self.old_width = 0
        self.lead_chars = 0


def leading_width(text):
    w = 0
    for ch in text:
        if ch == " ":
            w += 1
        elif ch == "\t":
            w += TAB_STOP - (w % TAB_STOP)
        else:
            break
    return w


def expand_tabs(text, protected_spans):
    """Expand tabs to spaces at 8-column stops, skipping protected spans.

    protected_spans: list of (start_off, end_off) character offsets within
    text whose bytes must not be altered (string/char literal content).
    """
    if "\t" not in text:
        return text
    out = []
    col = 0
    for idx, ch in enumerate(text):
        if any(s <= idx < e for (s, e) in protected_spans):
            out.append(ch)
            col += 1
            continue
        if ch == "\t":
            k = TAB_STOP - (col % TAB_STOP)
            out.append(" " * k)
            col += k
        else:
            out.append(ch)
            col += 1
    return "".join(out)


def build_lines(src, tokens):
    """Classify every physical line and attach tokens / protection info."""
    raw_lines = src.split("\n")
    lines = [Line(i + 1, r) for i, r in enumerate(raw_lines)]

    protected = {}  # line_no -> [(start_off, end_off), ...]
    code_tokens = [t for t in tokens if t.kind == "code"]
    code_index = {id(t): i for i, t in enumerate(code_tokens)}
    preproc_starts = {t.start_line for t in tokens if t.kind == "preproc"}
    string_starts = {t.start_line for t in tokens
                     if t.kind in ("string", "char")}

    for t in tokens:
        for ln in range(t.start_line, t.end_line + 1):
            if not (1 <= ln <= len(lines)):
                continue
            line = lines[ln - 1]
            if t.kind == "code":
                line.code_idx.append(code_index[id(t)])
            # Every line after the first of a multi-line token starts inside
            # the token; literal interiors must keep their exact bytes.
            if ln > t.start_line and t.kind in ("string", "char"):
                line.untouchable = True
        if t.kind in ("string", "char"):
            for (ln, s, e) in t.spans:
                if 1 <= ln <= len(lines):
                    protected.setdefault(ln, []).append((s, e))

    # Phase 1: structural facts (opaque / blank / preproc directive start).
    for line in lines:
        line.old_width = leading_width(line.raw)
        stripped = line.raw.lstrip(" \t")
        line.lead_chars = len(line.raw) - len(stripped)
        line.exp = expand_tabs(line.raw, protected.get(line.no, []))
        if line.untouchable:
            line.kind = "opaque"
        elif line.raw.strip() == "":
            line.kind = "blank"
        elif stripped.startswith("#") and line.no in preproc_starts:
            line.kind = "preproc"

    # Phase 2: preprocessor continuation lines.
    for t in tokens:
        if t.kind == "preproc" and t.end_line > t.start_line:
            for ln in range(t.start_line + 1, t.end_line + 1):
                if 1 <= ln <= len(lines) and lines[ln - 1].kind == "":
                    lines[ln - 1].kind = "preproc_cont"

    # Phase 3: everything else is code, a comment, or (degenerate) blank.
    for line in lines:
        if line.kind:
            continue
        if line.code_idx:
            line.kind = "code"
        elif line.no in string_starts:
            line.kind = "code"   # line of bare string/char literal(s)
        elif (line.raw.strip().startswith(("//", "/*"))
              or any(t.kind in ("icomment", "bcomment") and t.start_line == line.no
                     for t in tokens)):
            line.kind = "comment"
        else:
            line.kind = "blank"

    return lines, code_tokens


def _mark_soft_blocks(lines, tokens):
    """Group multi-line block comments so interior lines shift with the
    comment's first line (their relative layout is preserved)."""
    for t in tokens:
        if t.kind not in ("bcomment", "icomment") or t.end_line <= t.start_line:
            continue
        head = lines[t.start_line - 1]
        for ln in range(t.start_line + 1, t.end_line + 1):
            if 1 <= ln <= len(lines):
                line = lines[ln - 1]
                if not line.untouchable:
                    line.soft_block_head = head
                    if line.kind == "blank":
                        line.kind = "soft"


def assign_soft_block_heads(lines):
    """Relabel 'soft' interiors as comment lines; they are emitted through
    their block head's delta either way."""
    for line in lines:
        if line.kind == "soft":
            line.kind = "comment"


# ---------------------------------------------------------------------------
# Continuation analysis
# ---------------------------------------------------------------------------

def analyze_continuations(lines, tokens, code_tokens):
    """Mark lines that continue the previous logical line.

    A line continues when it starts inside open parentheses/brackets, when
    the previous code line ends with an operator/comma that cannot end a
    statement, or when it is an adjacent string/char literal of the previous
    line (implicit concatenation).  Continuations keep their relative
    alignment: their indent shifts by the same delta as their anchor line.
    Structural lines ('}', '{', 'case', 'default') never count as
    continuations.
    """
    by_line = {}
    for t in tokens:
        by_line.setdefault(t.start_line, []).append(t)

    paren = 0
    paren_at_start = {}
    for line in lines:
        paren_at_start[line.no] = paren
        for idx in sorted(line.code_idx):
            t = code_tokens[idx].text
            if t in ("(", "["):
                paren += 1
            elif t in (")", "]"):
                paren = max(0, paren - 1)

    prev_code = None
    prev_ends_lit = False
    for line in lines:
        if line.kind == "blank":
            continue
        if line.kind in ("preproc", "preproc_cont"):
            prev_code = None
            prev_ends_lit = False
            continue
        ts = [t for t in by_line.get(line.no, [])
              if t.kind not in ("icomment", "bcomment")]
        starts_lit = bool(ts) and ts[0].kind in ("string", "char")
        first_tok = first_code_text(line, code_tokens)
        structural = first_tok in ("}", "{", "case", "default")
        cont = False
        if not structural:
            cont = paren_at_start.get(line.no, 0) > 0
            if (not cont and prev_code is not None
                    and not (prev_code.is_case or prev_code.is_label)
                    and prev_code.last_code_text(code_tokens) in CONTINUATION_END_TOKENS):
                cont = True
            if not cont and prev_ends_lit and starts_lit:
                cont = True
            # Ternary / logical operators opening a line continue the
            # previous one (they cannot start a statement themselves).
            if not cont and prev_code is not None and first_tok in ("?", ":", "||", "&&"):
                cont = True
            if prev_code is None:
                cont = False
        if line.kind == "code":
            line.is_cont = cont
        elif line.kind == "comment":
            line.is_cont = cont
        if line.code_idx:
            prev_code = line
        if ts:
            prev_ends_lit = ts[-1].kind in ("string", "char")


def _last_code_text(self, code_tokens):
    if not self.code_idx:
        return None
    return code_tokens[max(self.code_idx)].text


Line.last_code_text = _last_code_text  # type: ignore[attr-defined]


def first_code_text(line, code_tokens):
    if not line.code_idx:
        return None
    return code_tokens[min(line.code_idx)].text


# ---------------------------------------------------------------------------
# Structural depth machine
# ---------------------------------------------------------------------------

class Frame:
    __slots__ = ("kind", "tag", "depth")

    def __init__(self, kind, tag, depth):
        self.kind = kind  # 'block' | 'virtual'
        self.tag = tag
        self.depth = depth


def preproc_word(text):
    """Directive word of a preprocessor token ('ifdef', 'else', ...)."""
    m = re.match(r"#\s*(\w+)", text)
    return m.group(1) if m else ""


def analyze_structure(lines, tokens):
    """Assign a structural indentation level to each line, using a stack of
    block frames ('{') and virtual frames (unbraced control bodies).

    Preprocessor conditionals are tracked so alternative branches do not
    inflate nesting: '#else'/'#elif' rewind to the state at the '#if', and
    '#endif' settles on the deepest branch state, so a brace opened by
    every branch (a split function definition) stays open after it.
    """
    stoks = [t for t in tokens if t.kind in ("code", "preproc")]
    stok_lines = [lines[t.start_line - 1] for t in stoks]

    frames = []
    cond_stack = []            # preproc conditional: saved state + branch ends
    i, n = 0, len(stoks)
    last_closed_do = False
    pending_header = None      # dict(tag=...) while inside a control header
    header_paren = 0           # paren depth within the current header
    expect_do_block = False    # 'do' seen; the next '{' opens a do-block
    last_popped_if = None      # virtual if-frame popped at the last ';'
                               # (a following 'else' binds to it)

    def cur_depth():
        return frames[-1].depth if frames else 0

    def peek(k):
        for j in range(k + 1, n):
            if stoks[j].kind == "code":
                return stoks[j].text
        return None

    def frames_at_depth(fr):
        return fr[-1].depth if fr else 0

    def pop_virtuals():
        # Remember the innermost if-frame being popped so that a following
        # 'else' can bind to it (dangling-else rule): 'if (a)\n if (b)\n x();\n
        # else' — the else belongs to the inner if.
        nonlocal last_popped_if
        if frames and frames[-1].kind == "virtual" and frames[-1].tag == "if":
            last_popped_if = frames[-1]
        while frames and frames[-1].kind == "virtual":
            frames.pop()

    while i < n:
        tok = stoks[i]
        if tok.kind == "preproc":
            word = preproc_word(tok.text)
            if word in ("if", "ifdef", "ifndef"):
                cond_stack.append({"saved": list(frames), "ends": []})
            elif word in ("else", "elif") and cond_stack:
                node = cond_stack[-1]
                node["ends"].append(list(frames))
                frames = list(node["saved"])
            elif word == "endif" and cond_stack:
                node = cond_stack.pop()
                node["ends"].append(list(frames))
                best = None
                for end in node["ends"]:
                    if best is None or frames_at_depth(end) > frames_at_depth(best):
                        best = end
                if best is not None:
                    frames = list(best)
            i += 1
            continue
        text = tok.text
        # A pending dangling-else binding survives only up to the next
        # token; anything but 'else' invalidates it.
        if text != "else":
            last_popped_if = None
        ln = stok_lines[i]
        do_just_closed = last_closed_do
        last_closed_do = False
        do_flag = expect_do_block
        expect_do_block = False

        # Inside a control header's parentheses: wait for the matching ')'.
        if pending_header is not None:
            if text == "(":
                header_paren += 1
            elif text == ")":
                header_paren = max(0, header_paren - 1)
                if header_paren == 0:
                    tag = pending_header["tag"]
                    pending_header = None
                    if tag != "while-do" and peek(i) != "{":
                        frames.append(Frame("virtual", tag, cur_depth() + 1))
            i += 1
            continue

        if ln.level is None and ln.kind == "code":
            # First significant token of this line sets its base level.
            if text == "}":
                pass  # assigned after the pop, below
            elif text in ("case", "default"):
                ln.is_case = True
                ln.level = max(0, cur_depth() - 1)
            elif text == "else" and last_popped_if is not None:
                # Dangling else: the pop at the last ';' removed the body
                # frame of the if this else binds to — align with that if.
                ln.level = max(0, last_popped_if.depth - 1)
            elif (peek(i) == ":"
                  and text.isidentifier() and text not in ALL_KEYWORDS):
                ln.is_label = True
                ln.level = max(0, cur_depth() - 1)
            else:
                ln.level = cur_depth()

        if text in CONTROL_KEYWORDS:
            if text == "while" and do_just_closed:
                pending_header = {"tag": "while-do"}
                header_paren = 0
                i += 1
                continue
            pending_header = {"tag": text}
            header_paren = 0
            i += 1
            continue

        if text == "do":
            if peek(i) == "{":
                expect_do_block = True
            else:
                frames.append(Frame("virtual", "do", cur_depth() + 1))
            i += 1
            continue

        if text == "else":
            nxt = peek(i)
            same_line_next = (i + 1 < n and stok_lines[i + 1].no == ln.no)
            if nxt == "{":
                pass
            elif nxt == "if" and same_line_next:
                pass  # '} else if (...)' — the if manages its own body
            else:
                bind = last_popped_if
                body_depth = (bind.depth if bind is not None
                              else cur_depth() + 1)
                frames.append(Frame("virtual", "else", body_depth))
            last_popped_if = None
            i += 1
            continue

        if text == "{":
            frames.append(Frame("block", "do" if do_flag else "block",
                                cur_depth() + 1))
            i += 1
            continue

        if text == "}":
            while frames and frames[-1].kind == "virtual":
                frames.pop()
            was_do = False
            if frames and frames[-1].kind == "block":
                was_do = frames[-1].tag == "do"
                frames.pop()
            if ln.kind == "code" and ln.level is None:
                ln.level = max(0, cur_depth())
            nxt = peek(i)
            if not (nxt == "else" or (nxt == "while" and was_do)):
                pop_virtuals()
            if was_do:
                ln.do_close = True
            last_closed_do = was_do
            i += 1
            continue

        if text == ";":
            pop_virtuals()

        i += 1


# ---------------------------------------------------------------------------
# Comment levels
# ---------------------------------------------------------------------------

def analyze_comments(lines, code_tokens):
    """Assign levels to comment-only lines: a comment conventionally sits at
    the level of the code it introduces (just inside a closing brace)."""
    n = len(lines)
    for i, line in enumerate(lines):
        if line.kind != "comment" or line.is_cont:
            continue
        target = None
        for j in range(i + 1, n):
            nxt = lines[j]
            if nxt.merged_into is not None or nxt.is_cont:
                continue
            if nxt.kind in ("preproc", "preproc_cont"):
                continue
            if nxt.kind == "code":
                target = nxt
                break
        if target is not None and target.level is not None:
            if first_code_text(target, code_tokens) == "}":
                line.level = target.level + 1
            else:
                line.level = target.level
        elif line.level is None:
            line.level = 0


# ---------------------------------------------------------------------------
# Brace joining (jams)
# ---------------------------------------------------------------------------

def _group_parts(line):
    return [line] + line.parts


def group_tokens(line, code_tokens):
    toks = []
    for p in _group_parts(line):
        toks.extend(code_tokens[idx].text for idx in sorted(p.code_idx))
    return toks


def group_last_text(line):
    for p in reversed(_group_parts(line)):
        if p.exp.strip():
            return p.exp.strip()
    return ""


def group_ends_with_backslash(line):
    return group_last_text(line).endswith("\\")


def analyze_jams(lines, code_tokens):
    """Merge Allman braces, else/while continuations and GNU function
    headers, in that order of precedence."""
    n = len(lines)

    def resolve(line):
        while line.merged_into is not None:
            line = line.merged_into
        return line

    def prev_nonblank_idx(i):
        j = i - 1
        while j >= 0 and lines[j].kind == "blank":
            j -= 1
        return j if j >= 0 else None

    def next_nonblank_idx(i):
        j = i + 1
        while j < n and lines[j].kind == "blank":
            j += 1
        return j if j < n else None

    def absorb(carrier, part):
        """Merge `part` (with anything already merged onto it) into
        `carrier`; the merged text is emitted as one output line."""
        part.merged_into = carrier
        carrier.parts.append(part)
        for q in part.parts:
            q.merged_into = carrier
            carrier.parts.append(q)
        part.parts = []

    # --- J3: function return type + name on one line ----------------------
    type_only = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t*]*$")
    name_start = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*\s*\(")
    type_words = TYPE_HEADER_KEYWORDS | {
        "static", "extern", "const", "unsigned", "signed", "void", "int",
        "char", "long", "short", "float", "double", "bool", "size_t", "inline",
    }
    for i, line in enumerate(lines):
        if line.kind != "code" or line.merged_into is not None or line.is_cont:
            continue
        text = line.exp.strip()
        if not text or line.parts:
            continue
        if not type_only.match(text):
            continue
        first_word = re.match(r"[A-Za-z_][A-Za-z0-9_]*", text)
        if not ((first_word and first_word.group(0) in type_words)
                or text.endswith("*")):
            continue
        j = next_nonblank_idx(i)
        if j is None:
            continue
        nxt = lines[j]
        if nxt.kind != "code" or nxt.merged_into is not None or nxt.is_cont:
            continue
        if not name_start.match(nxt.exp.strip()):
            continue
        absorb(line, nxt)

    # --- J1: opening brace joins the statement ----------------------------
    warrant_end = {")", "else", "do", "=", ":", "["}
    for i, line in enumerate(lines):
        if line.kind != "code" or line.merged_into is not None:
            continue
        text = line.exp.strip()
        if not text.startswith("{") or line.parts or line.is_cont:
            continue
        pi = prev_nonblank_idx(i)
        if pi is None:
            continue
        prev = resolve(lines[pi])
        if prev.kind != "code" or group_ends_with_backslash(prev):
            continue
        toks = group_tokens(prev, code_tokens)
        if not toks:
            continue
        warrants = toks[-1] in warrant_end
        if (not warrants and any(t in TYPE_HEADER_KEYWORDS for t in toks)
                and ";" not in toks and ")" not in toks):
            warrants = True
        if not warrants:
            continue
        absorb(prev, line)

    # --- J2: '}' joins else / else-if / do-while --------------------------
    for i, line in enumerate(lines):
        if line.kind != "code" or line.merged_into is not None:
            continue
        text = line.exp.strip()
        if not text.startswith("}"):
            continue
        rest = text[1:].strip()
        if rest and not rest.startswith(("/*", "//")):
            continue
        j = next_nonblank_idx(i)
        if j is None:
            continue
        nxt = lines[j]
        if nxt.kind != "code" or nxt.merged_into is not None:
            continue
        ntoks = group_tokens(nxt, code_tokens)
        if not ntoks:
            continue
        if ntoks[0] == "else":
            absorb(line, nxt)
            # Chain: '} else' + 'if (...)' joins the following if line too.
            if len(ntoks) == 1:  # a bare 'else' — its body starts below
                j2 = next_nonblank_idx(j)
                if j2 is not None:
                    m = lines[j2]
                    if m.kind == "code" and m.merged_into is None:
                        mtoks = group_tokens(m, code_tokens)
                        if mtoks and mtoks[0] == "if":
                            absorb(line, m)
        elif ntoks[0] == "while" and line.do_close:
            absorb(line, nxt)


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

def _strip_keep_splice(text):
    """Strip leading/trailing whitespace but never disturb a line whose
    final character is a backslash (it is a splice and must stay intact)."""
    if text.rstrip().endswith("\\"):
        return text.lstrip(" \t")
    return text.strip()


def compute_widths(lines):
    """Compute the new indentation width for every carrier line and the
    delta each one implies for its followers."""
    for line in lines:
        if line.merged_into is not None:
            continue
        if line.kind in ("code", "comment", "soft"):
            if line.level is not None:
                new_w = max(0, line.level) * INDENT_WIDTH
            else:
                new_w = line.old_width   # no structural position: stay put
        elif line.kind == "preproc":
            new_w = 0
        elif line.kind == "preproc_cont":
            new_w = line.old_width
        else:  # blank / opaque
            new_w = line.old_width
        line.delta = new_w - line.old_width

    # Followers: continuations shift with their anchor; comment-block
    # interiors shift with their block head.
    for idx, line in enumerate(lines):
        if line.merged_into is not None:
            continue
        if line.is_cont:
            anchor = line.shift_from
            if anchor is None:
                anchor = _find_anchor(lines, idx)
                line.shift_from = anchor
            if anchor is not None and anchor.merged_into is None:
                line.delta = anchor.delta
        if line.soft_block_head is not None:
            line.delta = line.soft_block_head.delta


def _find_anchor(lines, idx):
    for j in range(idx - 1, -1, -1):
        cand = lines[j]
        if cand.kind == "code" and not cand.is_cont and cand.merged_into is None:
            return cand
    return None


def emit_line(line):
    """Return the final text of a carrier output line (no trailing newline)."""
    if line.untouchable:
        return line.raw  # literal interior: leave the bytes alone
    pieces = []
    for p in _group_parts(line):
        pieces.append(_strip_keep_splice(p.exp))
    text = " ".join(pc for pc in pieces if pc)

    if line.kind == "blank":
        return ""
    if text == "":
        return ""

    width = max(0, line.old_width + line.delta)
    if not text.rstrip().endswith("\\"):
        text = text.rstrip()
    return " " * width + text


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def format_source(src, verify=True):
    """Format C source text and return the formatted text."""
    if src == "":
        return src
    tokens = tokenize(src)
    lines, code_tokens = build_lines(src, tokens)

    _mark_soft_blocks(lines, tokens)
    analyze_structure(lines, tokens)
    analyze_continuations(lines, tokens, code_tokens)
    analyze_comments(lines, code_tokens)
    assign_soft_block_heads(lines)
    analyze_jams(lines, code_tokens)
    compute_widths(lines)

    out_lines = []
    for line in lines:
        if line.merged_into is not None:
            continue
        out_lines.append(emit_line(line))
    out = "\n".join(out_lines)
    # Exactly one final newline.
    out = out.rstrip("\n") + "\n"

    if verify:
        fmt_tokens = tokenize(out)
        if not tokens_equal(tokens, fmt_tokens):
            raise AssertionError(
                "internal error: formatting changed the token stream; "
                "file not written"
            )
    return out


def check_source(src):
    """Return (needs_change, formatted) for verification/CI use."""
    fmt = format_source(src)
    return (fmt != src, fmt)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def find_files(paths):
    out = []
    for p in paths:
        if os.path.isfile(p):
            if p.endswith((".c", ".h")):
                out.append(p)
            continue
        for root, dirs, files in os.walk(p):
            dirs[:] = [d for d in dirs if d not in EXCLUDE_DIRS]
            for f in sorted(files):
                if f.endswith((".c", ".h")):
                    out.append(os.path.join(root, f))
    out = [p for p in out if _repo_rel(p) not in EXCLUDE_FILES]
    out = [p for p in out
           if not any(_repo_rel(p).startswith(pre) for pre in EXCLUDE_PREFIXES)]
    return sorted(set(out))


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="cstyle",
        description="HobbyOS C style formatter (see STYLE.md)",
    )
    ap.add_argument("command", choices=["format", "check", "diff"])
    ap.add_argument("paths", nargs="*", default=None,
                    help="files or directories (default: src)")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv)

    paths = args.paths or DEFAULT_PATHS
    files = find_files(paths)
    if not files:
        print("cstyle: no .c/.h files found", file=sys.stderr)
        return 2

    changed = 0
    failed = 0
    for path in files:
        try:
            with open(path, "r", encoding="utf-8") as fh:
                src = fh.read()
            fmt = format_source(src)
        except AssertionError as exc:
            print(f"cstyle: {path}: {exc}", file=sys.stderr)
            failed += 1
            continue
        except Exception as exc:  # pragma: no cover
            print(f"cstyle: {path}: error: {exc}", file=sys.stderr)
            failed += 1
            continue
        if fmt != src:
            changed += 1
            if args.command == "format":
                with open(path, "w", encoding="utf-8") as fh:
                    fh.write(fmt)
                if not args.quiet:
                    print(f"formatted {path}")
            elif args.command == "diff":
                d = difflib.unified_diff(
                    src.splitlines(keepends=True),
                    fmt.splitlines(keepends=True),
                    fromfile=path, tofile=path + " (formatted)")
                sys.stdout.writelines(d)
            elif not args.quiet:
                print(f"would reformat {path}")

    if failed:
        print(f"cstyle: {failed} file(s) failed verification", file=sys.stderr)
        return 2
    if args.command == "check":
        if changed:
            print(f"cstyle: {changed} file(s) need formatting", file=sys.stderr)
            return 1
        if not args.quiet:
            print(f"cstyle: all {len(files)} file(s) are properly formatted")
        return 0
    if not args.quiet:
        print(f"cstyle: {changed} file(s) updated, {len(files)} checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
