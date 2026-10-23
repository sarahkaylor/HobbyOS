import sys
p = "/tmp/gb084/src/user/sh.c"
s = open(p).read()
old = 'if (r->mode == 0) fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);'
new = 'if (r->mode == 0) fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC);'
assert s.count(old) == 1
open(p, "w").write(s.replace(old, new))
print("reverted one call site to 2-arg")
