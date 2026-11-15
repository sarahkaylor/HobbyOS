#!/usr/bin/env python3
"""tools/cp/p0host/vksum.py <vkdump.txt> — condense the vkdump layer log.

Prints every distinct vkCreateInstance (app/engine/apiVersion/extensions) and every distinct
vkCreateDevice block (physical device, queues, extensions, enabled features: core
VkPhysicalDeviceFeatures + each *Features struct's true members), each with how many times
it occurred.  Counts at the end: device extensions, feature structs, true feature bits."""
import sys, collections

blocks, cur = [], None
for l in open(sys.argv[1]):
    if l.startswith("=== "):
        cur = [l.split()[1]]; blocks.append(cur)
    elif cur is not None and not l.startswith("  result="):
        cur.append(l.rstrip("\n"))
seen = collections.OrderedDict()
for b in blocks:
    k = tuple(b)
    seen[k] = seen.get(k, 0) + 1
last_dev = None
for k, n in seen.items():
    print(f"### {k[0]} x{n}")
    for l in k[1:]: print(l)
    if k[0] == "vkCreateDevice": last_dev = k
if last_dev:
    ext = [l for l in last_dev if l.startswith("  dext ")]
    feat = [l for l in last_dev if l.startswith("  pNext ") or "features:" in l.lower() or l.startswith("  pEnabledFeatures")]
    bits = sum(len(l.split(":", 1)[1].split()) for l in feat if ":" in l and "(none true)" not in l)
    print(f"\n# summary of the last vkCreateDevice: {len(ext)} device extensions, "
          f"{len(feat)} feature structs in the chain, {bits} feature bits set")
