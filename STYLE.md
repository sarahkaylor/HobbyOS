# HobbyOS C Style Guide

This is the C formatting standard for the HobbyOS code base. It applies to
every first-party `.c` and `.h` file under `src/`. Vendored third-party code
(`third_party/`, `bootloader/`) is out of scope and keeps its upstream
formatting.

The rules are enforced mechanically by [`tools/cstyle.py`](tools/cstyle.py) —
run `make format` to auto-format, `make check-format` to verify. Do not
hand-format around the tool; if the tool and this guide ever disagree, fix
the tool.

## 1. Indentation

- **Spaces only, never tabs.** A tab is only acceptable as content *inside*
  a string or character literal (`"\t"` or a literal tab byte in a string);
  everywhere else the formatter expands tabs to spaces at 8-column stops.
- **Two spaces per indent level.** Never four, never eight.
- Each nesting level of braces and each unbraced control body adds exactly
  two spaces.

```c
int find_name(const char *name) {
  for (int i = 0; i < count; i++) {
    if (names[i] && strcmp(names[i], name) == 0) {
      return i;                                  /* level 3 = 6 spaces */
    }
  }
  return -1;
}
```

## 2. Brace placement (K&R)

- An **opening brace goes on the same line** as the statement that opens the
  block: `if`, `else`, `for`, `while`, `do`, `switch`, function definitions,
  `struct` / `enum` / `union` / `typedef` bodies, and initializer lists.
- A **closing brace sits on its own line**, except where it joins a
  continuation of the same statement: `} else {`, `} else if (...) {`,
  and the `while (...)` of a `do`-`while`.
- Never Allman braces (brace on its own line below the statement).

```c
if (ready) {
  start();
} else {
  wait();
}

for (int i = 0; i < n; i++) {
  step(i);
}

while (queued) {
  pop();
}

do {
  poll();
} while (!done);

switch (mode) {
case FAST:
  run();
  break;
default:
  idle();
  break;
}

struct point {
  int x;
  int y;
};

struct point origin = { 0, 0 };
```

## 3. Functions

- The return type and the function name stay on **one line**; the opening
  brace closes that same line.

```c
static uint32_t read_counter(void) {
  return counter;
}
```

## 4. Control statements

- The condition is wrapped in parentheses; `if`/`for`/`while` bodies of a
  single statement may omit braces (the standard permits it — the tool
  neither adds nor removes braces), but the single statement is still
  indented one level:

```c
if (ptr == NULL)
  return -1;
```

- `case` / `default` labels are indented to the same level as the `switch`
  keyword, and their bodies one level in (Linux-kernel convention).
- `goto` labels sit one level out from the statement flow they belong to,
  at the start of the line.
- A `case` label's body does not need braces.

## 5. Preprocessor

- Directives (`#include`, `#define`, `#if`, ...) start at **column 0**, even
  when they appear inside a function.
- Multi-line macro bodies (`\` continuations) are byte-preserved: the tool
  expands leading whitespace only and never reflows them.
- Preprocessor conditionals never change indentation: alternate `#if` /
  `#else` branches are evaluated independently, so braces opened in each
  branch of a split function definition do not double-indent the shared
  body below `#endif`.

## 6. Whitespace details

- **No trailing whitespace** on any line (lines ending in a `\` splice are
  the only exception — the backslash must be the last byte).
- **Exactly one final newline**; LF line endings.
- Inside a line, whitespace is preserved; the formatter does not re-space
  expressions, re-wrap long lines, or split/join logical lines. Line
  structure only changes for brace placement (rule 2) and function headers
  (rule 3).

## 7. Continuation lines

- A line that continues the previous logical line (inside open parentheses,
  after a trailing operator or comma, an adjacent string literal, or a
  leading `?`, `:`, `||`, `&&`) keeps its **relative alignment**: when the
  anchor line re-indents, the continuation shifts by the same amount. It is
  never snapped to a fixed indent.

```c
int total = compute(a, b,
                    c, d);                 /* aligned under the anchor */

static const char *usage_text =
    "one\n"
    "two\n";                               /* implicit concatenation */
```

- String and character literal bytes are never altered — not the spacing
  inside quotes, not escapes, not literal tabs.

## 8. Comments

- Standalone comments take the indentation of the code line that follows
  them; a comment sitting just before a block's closing brace stays one
  level inside that block.
- The interior layout of a multi-line comment block is preserved: the
  relative indentation of continuation lines shifts as a unit with the
  block (trailing whitespace is still stripped, per §6).
- Trailing comments (`code;  /* note */`) stay on their line.

## 9. Enforcement

```sh
make format        # rewrite every first-party file in place
make check-format  # exit non-zero if anything is not formatted (CI)
python3 tools/cstyle.py diff src/foo.c   # preview a change
```

Safety: before writing, the formatter re-tokenizes its output and compares
it against the input token-by-token. The formatter can only ever change
whitespace and brace joins — if the token stream would change in any other
way, the file is left untouched and an error is reported.

Formatting is idempotent: formatting an already-formatted file is a no-op.
