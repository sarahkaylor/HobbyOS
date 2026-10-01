/*
 * RTTI_T.BIN — libc++abi RTTI acceptance (l3-rtti lane; browser.md §6 / L6).
 *
 * A crt0-style program (main()) compiled as freestanding C++23 against the
 * vendored libc++ headers with RTTI ON (-frtti: the ICU consumption mode),
 * linked against obj/<arch>/libcxx.a + libc.a.  It exercises the cast
 * machinery private_typeinfo.cpp ships as sources.txt class B:
 *
 *   dynamic_cast up / down / cross (second non-virtual base) / through a
 *   virtual base, the nullptr failure paths (unrelated class, sibling class,
 *   null source pointer), and typeid (dynamic type through a base pointer,
 *   inequality, mangled name, static type form).
 *
 * The four symbols this pins are exactly the ones the ICU 78.3 link probe
 * reported before the closure landed: __dynamic_cast and the vtables of
 * __class_type_info / __si_class_type_info / __vmi_class_type_info.
 *
 * Output convention (wave-scannable): "  RTTI_T <name> : PASS" per check,
 * then "ALL TESTS PASSED SUCCESSFULLY!" — or a FAIL line naming the check.
 * The process exits with the failure count.
 */
#include "libc.h"

#include <typeinfo>

static int checks;
static int fails;

static void check(const char *name, int ok) {
  checks++;
  print_console("  RTTI_T ");
  print_console(name);
  print_console(ok ? " : PASS\n" : " : FAIL\n");
  if (!ok)
    fails++;
}

/* Self-contained (no <string.h> dependency): compares to a literal. */
static int name_is(const char *s, const char *lit) {
  while (*s && *s == *lit) {
    s++;
    lit++;
  }
  return *s == *lit;
}

/* Leaf/Mid/Base: single inheritance.  Cross: two non-virtual bases (the
 * cross-cast case).  VLeaf/VBase/Base: virtual inheritance (the
 * __vmi_class_type_info case).  Stranger is unrelated: casts to it fail. */
struct Base {
  virtual ~Base() {}
  int b;
};
struct Other {
  virtual ~Other() {}
  int o;
};
struct Mid : Base {
  int m;
};
struct Leaf : Mid {
  int l;
};
struct Cross : Other, Base {
  int c;
};
struct VBase : virtual Base {
  int v;
};
struct VLeaf : VBase {
  int vl;
};
struct Stranger {
  virtual ~Stranger() {}
  int s;
};

extern "C" int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  print_console("[RTTI_T] libc++abi RTTI acceptance (dynamic_cast/typeid)\n");

  Leaf leaf;
  Base *bp = &leaf;
  Leaf *lp = &leaf;
  Mid *mp = &leaf;

  /* up / down / through the middle */
  check("upcast", dynamic_cast<Base *>(lp) == bp);
  check("downcast", dynamic_cast<Leaf *>(bp) == lp);
  check("downcast_mid", dynamic_cast<Mid *>(bp) == mp);

  /* failure paths return nullptr, never a bogus pointer */
  Stranger stranger;
  check("downcast_nullptr_unrelated", dynamic_cast<Leaf *>(&stranger) == 0);
  Cross cross;
  Base *cbp = &cross;
  check("downcast_nullptr_sibling", dynamic_cast<Leaf *>(cbp) == 0);
  check("nullptr_source", dynamic_cast<Leaf *>(static_cast<Base *>(0)) == 0);

  /* cross cast through a second non-virtual base and back */
  Other *op = dynamic_cast<Other *>(cbp);
  check("crosscast", op == static_cast<Other *>(&cross));
  check("crosscast_back", dynamic_cast<Base *>(op) == cbp);
  check("crosscast_nullptr", dynamic_cast<Other *>(bp) == 0);

  /* virtual base: up to Base and back down to the most-derived type */
  VLeaf vleaf;
  VLeaf *vlp = &vleaf;
  Base *vbp = dynamic_cast<Base *>(vlp);
  check("vbase_up", vbp == static_cast<Base *>(&vleaf));
  check("vbase_down", dynamic_cast<VLeaf *>(vbp) == vlp);

  /* typeid: dynamic type, inequality, mangled name, static form */
  check("typeid_dynamic", typeid(*bp) == typeid(Leaf));
  check("typeid_neq", !(typeid(*bp) == typeid(Cross)));
  check("typeid_name", name_is(typeid(*bp).name(), "4Leaf"));
  check("typeid_static", typeid(Leaf) == typeid(leaf));
  check("typeid_vbase", typeid(*vbp) == typeid(VLeaf));

  if (fails == 0)
    print_console("ALL TESTS PASSED SUCCESSFULLY!\n");
  else
    print_console("RTTI_T FAILED\n");
  exit(fails);
}
