import importlib.util, os, sys

p = os.path.abspath("tools/evidence_rig_test.py")
spec = importlib.util.spec_from_file_location("erigtest", p)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

stage_line, tier_line = m._read_live_snippets()
print("LEG A recipe line (read live from Makefile):")
print("  " + stage_line.strip())
print("LEG A runner: subprocess.run(['make', '-C', <tmp> 'run']) with that line in a throwaway Makefile")
print("             (QEMU_CMD = '@:' — stubbed), RUN_STAGE exported in env")
print()
print("LEG B line (tier invocation):")
print("  " + tier_line.strip())
print("LEG B runner: subprocess.run(['sh', '-c', 'ROOT=\"$ROOT\" sh -c \"$T3_STAGE\"'])")
print()
print("tier file line 183:", open("tools/t3_tier.sh").read().splitlines()[182].strip())