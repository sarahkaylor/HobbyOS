# GB-007 — differential harness skeleton
# tools/differential/: build + capture + compare the {stdout, exit, errno}
# contract of the `hello` probe across a reference (host) and a target
# (HobbyOS-intel / QEMU) run.  See hello.c, diff_harness.py, diff_harness_test.py.

# Build the probe (native host cc for the reference leg; set CC=<musl-cross>
# for the HobbyOS-intel differential leg from the same source).
DIFF_HELLO = tools/differential/build/hello
# diff_harness.py entry points (see --help).
DIFF_HARNESS = tools/differential/diff_harness.py
# T1 self-test: hello diverges / lie detector works.
DIFF_HARNESS_TEST = tools/differential/diff_harness_test.py

$(DIFF_HELLO): tools/differential/hello.c tools/differential/build_hello.sh
	bash tools/differential/build_hello.sh

diff_harness: $(DIFF_HELLO)
	@echo "run: ./tools/differential/diff_harness.py run --exe $(DIFF_HELLO) --out /tmp/ref.json"

test_diff_harness:
	python3 $(DIFF_HARNESS_TEST)
