/*
 * HobbyOS l3-rtti host test: the libc++abi RTTI closure (sources.txt class B)
 * compiled FOR THE HOST and driven through real compiler-emitted
 * dynamic_cast/typeid calls.  The objects under test are the shipped vendor
 * sources (obj/host_rtti_private_typeinfo.o and the two typeinfo owners), not
 * a re-implementation, and the link is closed-world (-nostdlib++: no host C++
 * runtime is imported) so a call can never silently fall through to a system
 * libstdc++/libc++abi implementation.
 *
 * private_typeinfo.cpp defines exactly the four symbols the ICU 78.3 link
 * probe reported for this lane: __dynamic_cast plus the vtables of
 * __class_type_info / __si_class_type_info / __vmi_class_type_info.  The
 * battery mirrors the on-device RTTI_T.BIN (ARM wave); this test adds the
 * boundary case the wave cannot observe — a failing dynamic_cast<T&> must
 * reach __cxa_bad_cast (asserted as a forked child dying of SIGABRT).
 *
 * The support TU (src/host/rtti_test_support.cpp) provides the four leaf
 * symbols the closure references but that a host binary has nowhere else to
 * get from: __abort_message, __cxa_pure_virtual, __cxa_bad_cast and the sized
 * operator delete.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <typeinfo>

static int total_checks = 0;
static int failures = 0;

#define CHECK(name, cond)                                                     \
    do {                                                                      \
        total_checks++;                                                       \
        printf("  RTTI_T %s : %s\n", name, (cond) ? "PASS" : "FAIL");         \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("    FAIL %s (line %d)\n", name, __LINE__);                \
        }                                                                     \
    } while (0)

/* ------------------------------------------------------------- the classes
 *
 * Leaf/Mid/Base: single inheritance.  Cross: two non-virtual bases (the
 * cross-cast case).  VLeaf/VBase/Base: virtual inheritance (the
 * __vmi_class_type_info case).  Stranger is unrelated to all of them: a
 * dynamic_cast to it must fail with nullptr.
 */
struct Base {
  virtual ~Base() {}
  int b = 1;
};
struct Other {
  virtual ~Other() {}
  int o = 2;
};
struct Mid : Base {
  int m = 3;
};
struct Leaf : Mid {
  int l = 4;
};
struct Cross : Other, Base {
  int c = 5;
};
struct VBase : virtual Base {
  int v = 6;
};
struct VLeaf : VBase {
  int vl = 7;
};
struct Stranger {
  virtual ~Stranger() {}
  int s = 8;
};

/* ------------------------------------------------ the reference-cast abort
 *
 * dynamic_cast<Leaf&> on a Cross object must fail: the compiler emits a
 * __cxa_bad_cast() call on the null return (noreturn — in the device build
 * cxa_aux_runtime.cpp's no-exceptions branch calls std::terminate(); the
 * host support shim prints and abort()s, same observable signal).
 */
static int child_ref_cast_failure(void) {
  Cross cross;
  Base *bp = &cross;
  (void)dynamic_cast<Leaf &>(*bp);
  return 0; /* not reached when the abort path works */
}

static int run_child(int (*fn)(void)) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
    _exit(fn());
  int status = 0;
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  return status;
}

int main(void) {
  Leaf leaf;
  Base *bp = &leaf;
  Leaf *lp = &leaf;
  Mid *mp = &leaf;

  printf("[rtti_test] libc++abi RTTI closure (host, -nostdlib++)\n");

  /* up / down / through the middle */
  CHECK("upcast", dynamic_cast<Base *>(lp) == bp);
  CHECK("downcast", dynamic_cast<Leaf *>(bp) == lp);
  CHECK("downcast_mid", dynamic_cast<Mid *>(bp) == mp);

  /* failure paths: unrelated class, sibling under the same base, and a
   * null source pointer — all must yield nullptr, never a bogus pointer */
  Stranger stranger;
  CHECK("downcast_nullptr_unrelated", dynamic_cast<Leaf *>(&stranger) == nullptr);
  Cross cross;
  Base *cbp = &cross;
  CHECK("downcast_nullptr_sibling", dynamic_cast<Leaf *>(cbp) == nullptr);
  CHECK("nullptr_source", dynamic_cast<Leaf *>(static_cast<Base *>(nullptr)) == nullptr);

  /* cross cast through a second non-virtual base and back */
  Other *op = dynamic_cast<Other *>(cbp);
  CHECK("crosscast", op == static_cast<Other *>(&cross));
  CHECK("crosscast_back", dynamic_cast<Base *>(op) == cbp);
  CHECK("crosscast_nullptr", dynamic_cast<Other *>(bp) == nullptr);

  /* virtual base: up to Base and back down to the most-derived type */
  VLeaf vleaf;
  VLeaf *vlp = &vleaf;
  Base *vbp = dynamic_cast<Base *>(vlp);
  CHECK("vbase_up", vbp == static_cast<Base *>(&vleaf));
  CHECK("vbase_down", dynamic_cast<VLeaf *>(vbp) == vlp);

  /* typeid: dynamic type through a base pointer, inequality, mangled name,
   * and the static type form */
  CHECK("typeid_dynamic", typeid(*bp) == typeid(Leaf));
  CHECK("typeid_neq", !(typeid(*bp) == typeid(Cross)));
  CHECK("typeid_name", strcmp(typeid(*bp).name(), "4Leaf") == 0);
  CHECK("typeid_static", typeid(Leaf) == typeid(leaf));
  CHECK("typeid_vbase", typeid(*vbp) == typeid(VLeaf));

  /* failing reference cast: forked child must abort (SIGABRT) */
  int st = run_child(child_ref_cast_failure);
  CHECK("ref_cast_failure_aborts",
        st >= 0 && WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);

  printf("rtti_test: %d checks, %d failures\n", total_checks, failures);
  if (failures == 0) {
    printf("RTTI TEST PASSED\n");
    return 0;
  }
  printf("RTTI TEST FAILED\n");
  return 1;
}
