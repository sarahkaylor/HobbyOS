/* ffi_smoke.c -- deterministic static-link smoke for vendored libffi 3.4.8
 * (L6 lane browser/l6-glib).
 *
 * Proves that libffi.a links standalone and exercises the two paths GLib's
 * GObject uses: ffi_call (calling a function through a prepared cif) and
 * ffi_closure (the trampoline behind g_cclosure_marshal_generic, the fallback
 * marshaller for every GObject signal closure).
 *
 * Output is deterministic; exits 0 and prints "ALL TESTS PASSED SUCCESSFULLY!"
 * as the final line on success. */
#include <ffi.h>
#include <stdio.h>

static unsigned checks = 0, failures = 0;
static void check(int ok, const char *name)
{
    checks++;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        failures++;
}

static int add3(int a, int b, int c) { return a + b + c; }
static double scale2(double x, double y) { return x * y; }

static void closure_fn(ffi_cif *cif, void *ret, void **args, void *user_data)
{
    (void)cif; (void)user_data;
    /* double the integer argument */
    *(int *)ret = *(int *)args[0] * 2;
}

int main(void)
{
    ffi_cif cif;
    ffi_status st;

    printf("libffi smoke (compile-time FFI_DEFAULT_ABI=%d)\n", FFI_DEFAULT_ABI);

    /* --- ffi_call: three int args, int return (gcc calling convention) ---- */
    ffi_type *args[3] = { &ffi_type_sint, &ffi_type_sint, &ffi_type_sint };
    int a = 7, b = 35, c = 2, result = 0;
    void *values[3] = { &a, &b, &c };
    st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_sint, args);
    check(st == FFI_OK, "ffi_prep_cif(sint32 x3)");
    ffi_call(&cif, FFI_FN(add3), &result, values);
    check(result == 44, "ffi_call add3(7,35,2) == 44");

    /* --- ffi_call: two doubles, double return (SSE path) ------------------ */
    ffi_type *dargs[2] = { &ffi_type_double, &ffi_type_double };
    double x = 6.0, y = 7.0, dres = 0.0;
    void *dvals[2] = { &x, &y };
    st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_double, dargs);
    check(st == FFI_OK, "ffi_prep_cif(double x2)");
    ffi_call(&cif, FFI_FN(scale2), &dres, dvals);
    check(dres == 42.0, "ffi_call scale2(6,7) == 42");

    /* --- ffi_closure: trampoline that doubles an int ---------------------- */
    ffi_closure *cl = NULL;
    void *code = NULL;
    cl = ffi_closure_alloc(sizeof(ffi_closure), &code);
    check(cl != NULL && code != NULL, "ffi_closure_alloc (executable memory)");
    if (cl != NULL && code != NULL) {
        ffi_type *cargs[1] = { &ffi_type_sint };
        st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_sint, cargs);
        check(st == FFI_OK, "ffi_prep_cif(closure: sint32 -> sint32)");
        st = ffi_prep_closure_loc(cl, &cif, closure_fn, NULL, code);
        check(st == FFI_OK, "ffi_prep_closure_loc");
        {
            int (*fp)(int) = (int (*)(int))code;
            int got = fp(21);
            check(got == 42, "closure trampoline 21 -> 42");
        }
        ffi_closure_free(cl);
    }

    printf("%u checks, %u failures\n", checks, failures);
    if (failures == 0) {
        printf("ALL TESTS PASSED SUCCESSFULLY!\n");
        return 0;
    }
    printf("LIBFFI SMOKE FAILED\n");
    return 1;
}
