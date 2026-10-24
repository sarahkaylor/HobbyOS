p = "/tmp/gb096/src/kernel/arch/x64/cpu.c"
s = open(p).read()
old = """  uint64_t id;
  __asm__ volatile(\"mov %%gs:32, %0\" : \"=r\"(id));
  return (uint32_t)id;
}"""
new = """  uint64_t id;
  __asm__ volatile(\"mov %%gs:32, %0\" : \"=r\"(id));
  /* GB-096-V DIAGNOSTIC (not a fix): every caller indexes an 8-entry per-CPU
     array with this value.  A record shows cpu=23 on this VM, which is the
     hardware APIC id, not a dense CPU index -- so clamp to prove whether the
     out-of-range index is what corrupts a lock pointer. */
  if ((uint32_t)id >= 8u)
    return 0;
  return (uint32_t)id;
}"""
assert s.count(old) == 1
open(p, "w").write(s.replace(old, new))
print("clamped")
