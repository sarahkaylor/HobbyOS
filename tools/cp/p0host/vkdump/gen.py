#!/usr/bin/env python3
"""gen.py <vulkan_core.h> > features.inc — table of every *Features* struct whose members
after sType/pNext are all VkBool32: X(StructName) then F(StructName, member) lines."""
import re, sys
src = open(sys.argv[1]).read()
# only structs vk_struct_helper.hpp can map to an sType (GetSType<> specialisations)
# (skipping ones inside #ifdef platform/beta blocks, which are compiled out)
known, depth = set(), 0
for line in open("/usr/include/vulkan/utility/vk_struct_helper.hpp"):
    if re.match(r'\s*#\s*if', line) and "VK_STRUCT_HELPER" not in line: depth += 1
    elif re.match(r'\s*#\s*endif', line): depth = max(0, depth - 1)
    m = re.search(r'GetSType<(Vk\w+)>\(\)\s*\{', line)
    if m and depth == 0: known.add(m.group(1))
for m in re.finditer(r'typedef struct (Vk\w*Features\w*) \{(.*?)\} \1;', src, re.S):
    name, body = m.group(1), m.group(2)
    mem = [l.strip().rstrip(';') for l in body.strip().splitlines() if l.strip()]
    if len(mem) < 3 or not mem[0].startswith("VkStructureType") or "pNext" not in mem[1]:
        continue
    if name not in known:
        continue
    fields = mem[2:]
    if not all(f.startswith("VkBool32 ") for f in fields):
        continue
    print(f"BEGIN({name})")
    for f in fields:
        print(f"F({name}, {f.split()[1]})")
    print(f"END({name})")
