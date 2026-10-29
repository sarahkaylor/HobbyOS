#!/usr/bin/env python3
"""trap_exit_frame_order_test.py — GB-123: the trap-exit frame check must not
push into the frame it inspects.

Measured defect (GB-123 item 1, farm-1, `98fc494`): `common_trap_exit` advanced
rsp past the struct AND the vector/error slots, then did

    push rax
    push rcx
    call trap_exit_frame_check

so the two saves and the call's return address landed at [rsp+272], [rsp+264]
and [rsp+256] — the error, VECTOR and SPSR slots of the frame the callee reads.
A live instrumented wave showed the callee reading its own return address
(0x7003D87F) out of spsr_slot.

This test encodes the ordering invariant directly in the source of truth, so it
fails on the pre-fix asm and passes on the fixed asm:

  in every exit tail, `call trap_exit_frame_check` must appear BEFORE the
  `add rsp, 264` / `add rsp, 16` that advance past the frame, and the frame
  pointer handed to the check must be [rsp + 296] (the +240/+16 shift implied
  by pushing below the frame base).

It is static by design: a kernel asm ordering cannot be observed from a host
process, and the alternative (a live canary every wave) does not fail a build.
The live measurement lives in the GB-123 findings; this keeps it from regressing.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TRAP = os.path.join(ROOT, "src", "kernel", "arch", "x64", "trap.c")

fails = []


def check(cond, label, extra=""):
    if cond:
        print("PASS: " + label + (f" ({extra})" if extra else ""))
    else:
        fails.append(label)
        print("FAIL: " + label + (f" ({extra})" if extra else ""))


def tails(src):
    """Every `call trap_exit_frame_check` site, with the asm text before it."""
    out = []
    for m in re.finditer(r"call trap_exit_frame_check", src):
        out.append((m.start(), src[max(0, m.start() - 900):m.start()]))
    return out


def main():
    src = open(TRAP).read()
    sites = tails(src)
    check(len(sites) >= 2, "both exit tails still call the frame check",
          f"{len(sites)} site(s)")

    for i, (_pos, before) in enumerate(sites, 1):
        # the two adds that advance past the frame, in this tail
        adv = before.rfind('add rsp, 264')
        lea = before.rfind('lea rdi, [rsp + 296]')
        early_lea = before.rfind('lea rdi, [rsp + 16]')
        check(adv == -1,
              f"tail {i}: rsp is NOT advanced past the frame before the check")
        check(lea > 0 and lea > early_lea,
              f"tail {i}: the check gets [rsp + 296] (frame+280) from a rsp below the frame",
              f"lea@+{lea} early_lea@+{early_lea}")
        # and the advance must still exist somewhere after the call site
        after = src[_pos:_pos + 400]
        check('add rsp, 264' in after and 'add rsp, 16' in after,
              f"tail {i}: the frame is still consumed before iretq")

    print()
    if fails:
        print(f"TRAP_EXIT_FRAME_ORDER_TEST: FAIL — {len(fails)}")
        sys.exit(1)
    print("TRAP_EXIT_FRAME_ORDER_TEST: PASS")


if __name__ == "__main__":
    main()