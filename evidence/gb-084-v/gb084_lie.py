import sys
p = "/tmp/gb084/src/host/compat.c"
s = open(p).read()
old = "  if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }"
new = "  (void)0;  /* GB-084-V deliberate lie: ignore the supplied mode */"
assert s.count(old) == 1
open(p, "w").write(s.replace(old, new))
print("ho_open now ignores the supplied mode")
