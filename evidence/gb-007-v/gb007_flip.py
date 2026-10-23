import sys
p = "/tmp/gb007/src/host/compat.c"
s = open(p).read()
old = "  return open(filename, host, 0666);\n}\n\nint ho_dup(int fd) {"
new = "  return open(filename, host, 0666) + 1000;  /* GB-007-V deliberate lie */\n}\n\nint ho_dup(int fd) {"
assert s.count(old) == 1
open(p, "w").write(s.replace(old, new))
print("flipped ho_open return")
