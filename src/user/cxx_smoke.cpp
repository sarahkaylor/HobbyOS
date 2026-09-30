/*
 * CXXSMOKE.BIN — minimal C++ runtime acceptance (browser.md §6 F2.4).
 *
 * A crt0-style program (main())) built by clang as freestanding C++ with
 * -fno-exceptions -fno-rtti, linked against crt0.o + libc.a (which carries
 * the F2.4 runtime, src/libc/src/cxxrt.cpp).  Plain-language C++ features
 * only — the things an early port (Track A's small needs) uses:
 *
 *   1. static constructors:  .init_array collection + crt0 invocation.
 *      Ordering is asserted exactly: the init_priority(101) object first
 *      (SORT_BY_INIT_PRIORITY bucket), then the constructor-attribute
 *      function, then declaration order inside the TU thunk.  Runs on
 *      BOTH arches in the test wave, so the linker-script mechanism is
 *      proven by observed output, not by inspection.
 *   2. operator new/delete over the HobbyOS allocator: scalar, array,
 *      sized-delete path, zero-size, delete-nullptr, and a churn loop.
 *   3. function-local static with a dynamic initializer -> the
 *      __cxa_guard_acquire/release state machine (initialized exactly
 *      once), plus direct guard-primitive checks.
 *   4. virtual dispatch through an abstract base (vtable in .rodata, no
 *      RTTI involved).
 *   5. a small templated container (class template with operator[]).
 *   6. operator overloading (+, -, +=, ==) and placement new.
 *   7. __cxa_atexit registration and __cxa_finalize execution (also
 *      invoked automatically by exit() — see cxxrt.h).
 *   8. sysroot C functions are C++-callable: assert() from C++ resolves to
 *      the C _assert_fail (assert.h guards, F2.5 finding fixed), and the
 *      libc.h surface this file already links (print_console/print_dec/
 *      exit) proves the same for the rest.
 *
 * Output convention (wave-scannable): "  CXX_SMOKE <name>: PASS/FAIL" per
 * check, then "ALL TESTS PASSED SUCCESSFULLY!" or "CXX_SMOKE FAILED: n".
 * The process exits with the failure count.
 */

#include "libc.h"
#include "cxxrt.h"
#include "assert.h"

static int checks;
static int fails;

static void check(const char *name, int ok) {
  checks++;
  print_console("  CXX_SMOKE ");
  print_console(name);
  print_console(ok ? ": PASS\n" : ": FAIL\n");
  if (!ok)
    fails++;
}

/* ------- 1. static constructors: .init_array order + pre-main proof ---- */

/* volatile: the whole point of the test is that these writes happen at
 * .init_array time, before main.  Without volatile, -O2 can constant-fold
 * the entire constructor sequence into main's checks and delete the
 * initializers (observed during bring-up: the .init_array entry vanished
 * and the checks passed vacuously). */
static volatile char order_log[16];
static volatile int order_len;

static void log_order(char c) {
  if (order_len < 15)
    order_log[order_len++] = c;
  order_log[order_len] = 0;
}

struct Ordered {
  char id;
  bool born;
  Ordered(char c) : id(c), born(true) { log_order(c); }
};

/* init_priority(101): lands in .init_array.101, runs before every
 * default-priority initializer (the linker sorts .init_array.* first). */
static Ordered e_obj __attribute__((init_priority(101)))('E');

/* Plain constructor attribute: default-priority bucket, emitted directly
 * into .init_array (no TU thunk). */
__attribute__((constructor)) static void early_ctor(void) { log_order('0'); }

/* Declaration order inside the TU defines execution order inside the
 * _GLOBAL__sub_I_* thunk. */
static Ordered a_obj('A');
static Ordered b_obj('B');

/* A global with a non-trivial destructor: clang registers its dtor via
 * __cxa_atexit(&__dso_handle). */
static int tracked_dtors;
struct Tracked {
  int id;
  bool born;
  Tracked(int i) : id(i), born(true) { }
  ~Tracked() { tracked_dtors++; }
};
static Tracked global_tracked(7);

/* ------- 2. new/delete ------------------------------------------------ */

static void test_new_delete(void) {
  int *one = new int(1234);
  check("new-scalar", one != 0 && *one == 1234);
  delete one;

  int *arr = new int[64];
  for (int i = 0; i < 64; i++)
    arr[i] = i * 3;
  check("new-array", arr != 0 && arr[0] == 0 && arr[63] == 189);
  delete[] arr;

  char *zero = new char[0];
  check("new-zero-size-nonnull", zero != 0);
  delete[] zero;

  int *nul = 0;
  delete nul; /* must be a safe no-op */
  check("delete-nullptr", 1);

  /* operator new guarantees max_align_t alignment (16 on both targets).
   * The HobbyOS allocator returns 8-mod-16 pointers (24-byte block header),
   * so the runtime over-aligns; this check would catch a regression back to
   * pass-through allocation. */
  int *al1 = new int(1);
  int *al2 = new int[3];
  check("new-alignment-16",
        ((uintptr_t)al1 & 15) == 0 && ((uintptr_t)al2 & 15) == 0);
  delete al1;
  delete[] al2;

  /* Churn: allocator round-trips must stay consistent across many
   * new/delete pairs (sized-delete path included for non-trivial types).
   * Sum over round 0..63 of (p[0] + p[7]) = sum(2*round + 7) = 4480. */
  int total = 0;
  for (int round = 0; round < 64; round++) {
    int *p = new int[8];
    for (int i = 0; i < 8; i++)
      p[i] = round + i;
    total += p[0] + p[7];
    delete[] p;
  }
  check("new-delete-churn", total == 4480);
}

/* ------- 3. guards: function-local static, dynamic init once ---------- */

static int local_init_runs;

struct CountedInit {
  int v;
  CountedInit(int seed) : v(seed * 2) { local_init_runs++; }
};

static CountedInit &local_obj(void) {
  static CountedInit obj(21); /* dynamic init -> __cxa_guard_* */
  return obj;
}

static void test_guards(void) {
  /* direct primitive checks (Itanium ABI: 1 = caller must initialize) */
  static uint64_t probe_guard;
  check("guard-first-acquire", __cxa_guard_acquire(&probe_guard) == 1);
  __cxa_guard_release(&probe_guard);
  check("guard-second-acquire", __cxa_guard_acquire(&probe_guard) == 0);

  static uint64_t abort_guard;
  check("guard-abort-acquire", __cxa_guard_acquire(&abort_guard) == 1);
  __cxa_guard_abort(&abort_guard);
  check("guard-abort-retry", __cxa_guard_acquire(&abort_guard) == 1);
  __cxa_guard_release(&abort_guard);

  int before = local_init_runs;
  int v1 = local_obj().v;
  int v2 = local_obj().v;
  check("local-static-init-once",
        v1 == 42 && v2 == 42 && local_init_runs == before + 1);
}

/* ------- 4. virtual dispatch ------------------------------------------ */

struct Shape {
  int tag;
  Shape(int t) : tag(t) { }
  virtual int area() const = 0;
  virtual const char *name() const = 0;
  virtual ~Shape() { }
};

struct Square : Shape {
  int side;
  Square(int s) : Shape(1), side(s) { }
  int area() const override { return side * side; }
  const char *name() const override { return "square"; }
};

struct Rect : Shape {
  int w, h;
  Rect(int w, int h) : Shape(2), w(w), h(h) { }
  int area() const override { return w * h; }
  const char *name() const override { return "rect"; }
};

static int total_area(Shape *const *shapes, int n) {
  int sum = 0;
  for (int i = 0; i < n; i++)
    sum += shapes[i]->area(); /* virtual dispatch through the base */
  return sum;
}

static void test_virtual(void) {
  Square sq(5);
  Rect rc(3, 4);
  Shape *shapes[2] = {&sq, &rc};
  check("virtual-dispatch", total_area(shapes, 2) == 25 + 12);

  Shape &ref = sq;
  check("virtual-via-reference", ref.area() == 25 && ref.name()[0] == 's');

  Shape *poly = new Rect(2, 6);
  int a = poly->area();
  delete poly; /* virtual dtor through the base pointer */
  check("virtual-call-through-heap", a == 12);
}

/* ------- 5. class template -------------------------------------------- */

template <typename T, int N>
class FixedVec {
 public:
  FixedVec() : count(0) { }
  bool push(const T &v) {
    if (count >= N)
      return false;
    data[count++] = v;
    return true;
  }
  int size() const { return count; }
  T &operator[](int i) { return data[i]; }
  const T &operator[](int i) const { return data[i]; }

 private:
  T data[N];
  int count;
};

struct Point {
  int x, y;
};

static void test_template(void) {
  FixedVec<int, 4> ints;
  for (int i = 1; i <= 4; i++)
    ints.push(i * i);
  check("template-int-container",
        ints.size() == 4 && ints[0] == 1 && ints[3] == 16);
  check("template-overflow-rejected", !ints.push(99) && ints.size() == 4);

  FixedVec<Point, 2> pts;
  Point p = {3, 9};
  pts.push(p);
  check("template-struct-container", pts.size() == 1 && pts[0].y == 9);
}

/* ------- 6. operator overloading + placement new ----------------------- */

struct Vec2 {
  int x, y;
  Vec2(int x, int y) : x(x), y(y) { }
  Vec2 operator+(const Vec2 &o) const { return Vec2(x + o.x, y + o.y); }
  Vec2 operator-() const { return Vec2(-x, -y); }
  Vec2 &operator+=(const Vec2 &o) {
    x += o.x;
    y += o.y;
    return *this;
  }
  bool operator==(const Vec2 &o) const { return x == o.x && y == o.y; }
};

static void test_operators(void) {
  Vec2 a(1, 2), b(10, 20);
  Vec2 c = a + b;
  check("operator-plus", c == Vec2(11, 22));
  check("operator-minus-equals", (c += a) == Vec2(12, 24) && (-a) == Vec2(-1, -2));

  alignas(16) static unsigned char arena[sizeof(Vec2)];
  Vec2 *placed = new (arena) Vec2(4, 5); /* placement new */
  check("placement-new", placed == (Vec2 *)arena && placed->x == 4);
  placed->~Vec2();
}

/* ------- 7. __cxa_atexit / __cxa_finalize ------------------------------ */

static void test_atexit(void) {
  check("global-object-constructed", global_tracked.born && global_tracked.id == 7);

  int before = tracked_dtors;
  __cxa_finalize(0); /* explicit: run all registered destructors (LIFO) */
  check("cxa_finalize-runs-dtors", tracked_dtors == before + 1);

  int after = tracked_dtors;
  __cxa_finalize(0); /* idempotent: entries run at most once */
  check("cxa_finalize-idempotent", tracked_dtors == after);
}

/* ------- 8. sysroot C functions are C++-callable ---------------------- */

static void test_c_linkage(void) {
  /* F2.5: a C++ TU's assert() must link.  assert.h declares _assert_fail
   * inside extern "C" guards, so this emits a reference to the plain C
   * symbol; without the guards it would reference the mangled
   * _Z12_assert_fail... which nothing in libc.a defines (device link
   * error).  The volatile read keeps the branch - and therefore the symbol
   * reference - alive under -O2. */
  volatile int alive = 1;
  assert(alive == 1);
  check("assert-macro-cpp", 1);

  /* libc.h's declarations are the other half of the link path this binary
   * already proved (print_console/print_dec/exit above resolve to their C
   * definitions in libc.a). */
  check("sysroot-c-linkage", 1);
}

/* Entry point: crt0.c is C and calls `main`, but clang mangles the name in
 * freestanding C++ mode (held-out hosted special case) — so the definition
 * must be extern "C".  See the policy section of cxxrt.h. */
extern "C" int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  print_console("[CXX_SMOKE] minimal C++ runtime acceptance (F2.4)\n");

  /* 1. static ctors: exact .init_array order, all before main(). */
  check("init-array-before-main", order_len == 4);
  check("init-priority-first", order_len >= 1 && order_log[0] == 'E');
  check("init-array-order",
        order_len == 4 && order_log[1] == '0' && order_log[2] == 'A' &&
            order_log[3] == 'B');
  check("global-ctors-born", a_obj.born && b_obj.born && e_obj.born);

  test_new_delete();
  test_guards();
  test_virtual();
  test_template();
  test_operators();
  test_c_linkage();
  test_atexit();

  print_console("CXX_SMOKE checks: ");
  print_dec(checks);
  print_console(", failures: ");
  print_dec(fails);
  print_console("\n");

  if (fails == 0) {
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
    exit(0);
  }
  print_console("CXX_SMOKE FAILED\n");
  exit(fails);
}
