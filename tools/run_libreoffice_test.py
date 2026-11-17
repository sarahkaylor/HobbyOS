#!/usr/bin/env python3
"""
tools/run_libreoffice_test.py - Automated Host Test Runner for LibreOffice on HobbyOS

Responsibilities:
  1. Builds the boot disk (lo_test_build) without charging build time against the test budget.
  2. Launches QEMU with AArch64 virt machine (>=4 cores SMP), headless display (-display none),
     serial redirection, and a dedicated QMP control socket.
  3. Enforces the strict 30-second execution watchdog per GEMINI.md.
  4. Interacts via QMP to capture framebuffer screendumps and validate non-blank rendering.
  5. Parses serial logs for standardized [LO_TEST] markers across all 6 applications:
     Writer, Calc, Impress, Draw, Math, Base (78 required test cases).
  6. Detects kernel crashes, memory faults, lock violations, or unexpected reboot loops.
  7. Ensures clean QEMU process teardown and returns exit code 0 on all-pass, 1 on failure.

Usage:
  python3 tools/run_libreoffice_test.py [--arch arm|intel] [--timeout 30] [--no-build]
                                       [--screendump PATH] [--outdir DIR] [--verbose]
"""

import argparse
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


# ---------------------------------------------------------------------------
# QMP Protocol Client
# ---------------------------------------------------------------------------
class QmpClient:
    """Lightweight, robust client for the QEMU Machine Protocol (QMP)."""

    def __init__(self, socket_path: str, timeout: float = 10.0):
        self.socket_path = socket_path
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self._buf = b""
        self._connect()

    def _connect(self):
        deadline = time.time() + self.timeout
        connected = False
        while time.time() < deadline:
            try:
                self.sock.connect(self.socket_path)
                connected = True
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.2)
        if not connected:
            raise TimeoutError(f"Could not connect to QMP socket: {self.socket_path}")

        # Read greeting line
        self._readline()
        # Negotiate capabilities
        res = self.cmd("qmp_capabilities")
        if "return" not in res:
            raise RuntimeError(f"qmp_capabilities failed: {res}")

    def _readline(self) -> dict:
        try:
            while b"\n" not in self._buf:
                chunk = self.sock.recv(4096)
                if not chunk:
                    raise ConnectionResetError("QMP socket closed unexpectedly")
                self._buf += chunk
            line, self._buf = self._buf.split(b"\n", 1)
            return json.loads(line.decode("utf-8", errors="replace"))
        except (OSError, BrokenPipeError, ConnectionResetError) as e:
            return {"error": {"class": "ConnectionError", "desc": str(e)}}

    def cmd(self, execute: str, arguments: dict = None) -> dict:
        msg = {"execute": execute}
        if arguments:
            msg["arguments"] = arguments
        payload = json.dumps(msg).encode("utf-8") + b"\n"
        try:
            self.sock.sendall(payload)
        except (OSError, BrokenPipeError, ConnectionResetError) as e:
            return {"error": {"class": "ConnectionError", "desc": str(e)}}

        # QMP can emit asynchronous events before returning command response
        while True:
            resp = self._readline()
            if "return" in resp or "error" in resp:
                return resp

    def screendump(self, output_path: str) -> bool:
        """Capture guest framebuffer to a PPM file."""
        try:
            res = self.cmd("screendump", {"filename": output_path})
            if "error" in res:
                return False
            # Give disk write a brief moment to settle
            time.sleep(0.1)
            return os.path.exists(output_path) and os.path.getsize(output_path) > 100
        except (OSError, BrokenPipeError, ConnectionResetError, Exception):
            return False

    def query_status(self) -> dict:
        """Query VM run state."""
        return self.cmd("query-status").get("return", {})

    def system_powerdown(self) -> dict:
        """Issue ACPI powerdown event."""
        return self.cmd("system_powerdown")

    def quit(self) -> dict:
        """Force QEMU emulator termination."""
        return self.cmd("quit")

    def send_key(self, qcode: str):
        """Send keyboard event."""
        return self.cmd("send-key", {"keys": [{"type": "qcode", "data": qcode}]})

    def mouse_move(self, x: int, y: int, screen_w: int = 1920, screen_h: int = 1080):
        """Move pointer via virtio-tablet absolute coordinates (0..32767)."""
        abs_x = max(0, min(32767, int(x * 32767 / screen_w)))
        abs_y = max(0, min(32767, int(y * 32767 / screen_h)))
        return self.cmd("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": abs_x}},
            {"type": "abs", "data": {"axis": "y", "value": abs_y}},
        ]})

    def mouse_click(self, x: int, y: int, button: str = "left"):
        """Send mouse move + click sequence."""
        self.mouse_move(x, y)
        time.sleep(0.05)
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"button": button, "down": True}}]})
        time.sleep(0.05)
        self.cmd("input-send-event", {"events": [{"type": "btn", "data": {"button": button, "down": False}}]})

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# PPM Framebuffer Validator
# ---------------------------------------------------------------------------
class PpmValidator:
    """Validates binary PPM images captured from QEMU screendump."""

    @staticmethod
    def parse_header(data: bytes):
        if not data.startswith(b"P6"):
            return None, 0, 0
        idx = 2
        tokens = []
        while len(tokens) < 3 and idx < len(data):
            while idx < len(data) and data[idx:idx + 1].isspace():
                idx += 1
            if data[idx:idx + 1] == b"#":
                while idx < len(data) and data[idx:idx + 1] not in (b"\n", b""):
                    idx += 1
                continue
            start = idx
            while idx < len(data) and not data[idx:idx + 1].isspace():
                idx += 1
            if start < idx:
                try:
                    tokens.append(int(data[start:idx]))
                except ValueError:
                    return None, 0, 0
        if len(tokens) < 3:
            return None, 0, 0
        idx += 1  # single whitespace separator
        width, height, _ = tokens[0], tokens[1], tokens[2]
        return data[idx:], width, height

    @classmethod
    def validate(cls, ppm_path: str) -> tuple[bool, str]:
        if not os.path.exists(ppm_path):
            return False, f"PPM file does not exist: {ppm_path}"
        try:
            with open(ppm_path, "rb") as f:
                content = f.read()
        except OSError as e:
            return False, f"Failed to read PPM file: {e}"

        pixels, w, h = cls.parse_header(content)
        if pixels is None:
            return False, "Invalid or corrupted PPM header"
        if w <= 0 or h <= 0:
            return False, f"Invalid image dimensions ({w}x{h}, w<=0 or h<=0)"

        expected = w * h * 3
        if len(pixels) < expected:
            return False, f"Truncated PPM data: expected {expected} bytes, got {len(pixels)}"

        # Check for non-blank surface (must contain non-zero pixels)
        if not any(pixels[:expected]):
            return False, f"Screendump ({w}x{h}) is completely blank (all 0x00)"

        # Count distinct sampled colors to confirm meaningful visual content
        step = max(3, expected // 30000)
        colors = set()
        for off in range(0, expected - 3, step * 3):
            colors.add(pixels[off:off + 3])
            if len(colors) >= 8:
                break

        if len(colors) < 2:
            return False, "Screendump contains a single uniform color (flat display)"

        return True, f"Valid PPM ({w}x{h}, {len(colors)}+ colors detected)"


# ---------------------------------------------------------------------------
# Test Runner Engine
# ---------------------------------------------------------------------------
class LibreOfficeTestRunner:
    def __init__(self, args):
        self.args = args
        self.repo_dir = Path(os.path.abspath(__file__)).parent.parent
        self.out_dir = Path(args.outdir)
        self.serial_log_path = self.out_dir / "serial.log"
        self.ppm_shot_path = Path(args.screendump)
        self.qmp_sock_path = self.out_dir / "qmp-sock"
        self.timeout = float(args.timeout)

        # Required application suite passes
        self.expected_suites = [
            "WRITER",
            "CALC",
            "IMPRESS",
            "DRAW",
            "MATH",
            "BASE",
        ]
        self.passed_suites = set()
        self.fatal_errors = []

    def log(self, message: str):
        timestamp = time.strftime("%H:%M:%S")
        print(f"[{timestamp}] {message}", flush=True)

    def build_test_image(self) -> bool:
        """Build target disk.img without charging build time against test timeout."""
        self.log(f"Building HobbyOS test image for ARCH={self.args.arch}...")
        cmd = ["make", "lo_test_build", "MODE=lo_test", f"ARCH={self.args.arch}"]
        env = os.environ.copy()
        env["MTOOLS_SKIP_CHECK"] = "1"
        try:
            res = subprocess.run(
                cmd,
                cwd=self.repo_dir,
                capture_output=True,
                text=True,
                stdin=subprocess.DEVNULL,
                start_new_session=True,
                timeout=120,
                env=env,
            )
        except subprocess.TimeoutExpired as e:
            self.log(f"ERROR: Build command timed out after 120s: {e}")
            return False

        if res.returncode != 0:
            self.log("ERROR: Test image build failed!")
            print(res.stdout[-2000:])
            print(res.stderr[-2000:])
            return False
        self.log("Build completed successfully. Starting guest test execution timer.")
        return True

    def check_fatal_patterns(self, text: str):
        """Scans serial output for kernel faults, memory faults, and lock errors."""
        fatal_regexes = [
            (r"FATAL kernel-m|Exception in Kernel Mode", "Kernel mode fatal exception"),
            (r"\[KERNEL\] FATAL", "Kernel fatal assertion"),
            (r"\[KERNEL\].*memory fault", "Process memory fault"),
            (r"\[LOCKERR\]", "Lock ordering/hierarchy error"),
            (r"\[BLOCKHOLD\]", "Blocking while holding lock"),
            (r"\[MUTEX-SELF\]", "Recursive mutex deadlock"),
            (r"\[IDLESTUCK\]", "CPU failed to return to idle"),
            (r"\[LO_TEST\] FAIL:? (.*)", "In-guest test failure"),
        ]
        for pattern, desc in fatal_regexes:
            match = re.search(pattern, text)
            if match:
                err_detail = match.group(0)
                if desc not in [e[0] for e in self.fatal_errors]:
                    self.fatal_errors.append((desc, err_detail))
                    self.log(f"DETECTED FAULT: {desc} -> {err_detail}")

    def run(self) -> int:
        self.out_dir.mkdir(parents=True, exist_ok=True)
        if self.qmp_sock_path.exists():
            self.qmp_sock_path.unlink()
        if self.serial_log_path.exists():
            self.serial_log_path.unlink()

        # Step 1: Pre-build test image
        if not self.args.no_build:
            if not self.build_test_image():
                return 1

        # Step 2: Spawn QEMU process
        qemu_args = [
            "-display", "none",
            "-qmp", f"unix:{self.qmp_sock_path},server,nowait",
        ]
        make_cmd = [
            "make", "lo_test_run",
            "MODE=lo_test",
            f"ARCH={self.args.arch}",
            f"QEMU_ARGS={' '.join(qemu_args)}",
        ]

        self.log(f"Launching QEMU (watchdog deadline: {self.timeout}s)...")
        serial_file = open(self.serial_log_path, "w", buffering=1)
        start_time = time.time()

        proc = subprocess.Popen(
            make_cmd,
            cwd=self.repo_dir,
            stdout=serial_file,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )

        qmp = None
        all_suites_passed = False
        timed_out = False
        reboot_count = 0

        try:
            # Connect QMP client (bound by watchdog timeout)
            qmp_timeout = min(2.0, self.timeout / 4.0)
            try:
                qmp = QmpClient(str(self.qmp_sock_path), timeout=qmp_timeout)
                self.log("QMP client connected successfully.")
            except Exception as e:
                self.log(f"Warning: QMP connection failed ({e}). Proceeding with serial polling.")

            last_read_pos = 0
            serial_line_buffer = ""

            # Step 3: Main execution & watchdog loop
            while True:
                elapsed = time.time() - start_time
                if elapsed > self.timeout:
                    timed_out = True
                    self.log(f"ERROR: Watchdog timeout exceeded ({elapsed:.2f}s > {self.timeout}s)!")
                    break

                # Inspect serial log delta
                if self.serial_log_path.exists():
                    try:
                        with open(self.serial_log_path, "r", errors="ignore") as f:
                            f.seek(last_read_pos)
                            chunk = f.read()
                            last_read_pos = f.tell()

                        if chunk:
                            serial_line_buffer += chunk
                            if "\n" in serial_line_buffer:
                                complete_text, serial_line_buffer = serial_line_buffer.rsplit("\n", 1)
                                complete_text += "\n"

                                # Stream relevant lines to console
                                for line in complete_text.splitlines():
                                    line_s = line.strip()
                                    if line_s.startswith("[LO_TEST]") or "Mode:" in line_s or "System halt" in line_s:
                                        self.log(f"guest: {line_s}")

                                self.check_fatal_patterns(complete_text)

                                # Parse application passes
                                for suite in self.expected_suites:
                                    if f"[LO_TEST] {suite}: PASS" in complete_text:
                                        if suite not in self.passed_suites:
                                            self.passed_suites.add(suite)
                                            self.log(f"Verified Suite: {suite} PASSED ({len(self.passed_suites)}/6)")

                                if "[LO_TEST] ALL SUITES PASSED" in complete_text:
                                    all_suites_passed = True
                                    self.log("All 78 test cases verified passed in guest!")

                                # Check for reboot loop
                                reboot_count += complete_text.count("Controller reset complete")
                                if reboot_count > 2:
                                    self.fatal_errors.append(("Triple Fault Reboot Loop", "Guest restarted mid-run"))
                                    break

                    except OSError:
                        pass

                # If fatal error detected, break immediately
                if self.fatal_errors:
                    break

                # If all suites passed, allow brief settle for clean halt
                if all_suites_passed:
                    if proc.poll() is not None:
                        break
                    # Wait up to 2 seconds for clean halt
                    if elapsed > 1.0 and "System halt" in open(self.serial_log_path, errors="ignore").read():
                        break

                # Check if QEMU exited early
                if proc.poll() is not None:
                    break

                time.sleep(0.2)

            # Flush any remaining buffer text
            if serial_line_buffer:
                for line in serial_line_buffer.splitlines():
                    line_s = line.strip()
                    if line_s.startswith("[LO_TEST]") or "Mode:" in line_s or "System halt" in line_s:
                        self.log(f"guest: {line_s}")
                self.check_fatal_patterns(serial_line_buffer)
                for suite in self.expected_suites:
                    if f"[LO_TEST] {suite}: PASS" in serial_line_buffer:
                        if suite not in self.passed_suites:
                            self.passed_suites.add(suite)
                            self.log(f"Verified Suite: {suite} PASSED ({len(self.passed_suites)}/6)")
                if "[LO_TEST] ALL SUITES PASSED" in serial_line_buffer:
                    all_suites_passed = True

            # Step 4: Capture screendump for verification and diagnostics
            if qmp:
                try:
                    self.log(f"Capturing screendump to {self.ppm_shot_path}...")
                    if qmp.screendump(str(self.ppm_shot_path)):
                        ok, msg = PpmValidator.validate(str(self.ppm_shot_path))
                        self.log(f"Screendump verification: {msg}")
                        if not ok and all_suites_passed:
                            self.fatal_errors.append(("Framebuffer Error", msg))
                    else:
                        self.log("Warning: QMP screendump capture failed (guest VM may have already halted cleanly).")
                except Exception as e:
                    self.log(f"Warning: Screendump capture failed: {e}")

        finally:
            # Step 5: Clean process group teardown
            if qmp:
                try:
                    qmp.close()
                except Exception:
                    pass

            if proc.poll() is None:
                self.log("Terminating QEMU process group...")
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                    proc.wait(timeout=3.0)
                except Exception:
                    try:
                        self.log("Force killing QEMU process group...")
                        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                        proc.wait(timeout=2.0)
                    except Exception:
                        pass

            serial_file.close()
            qemu_execution_time = time.time() - start_time

        # Step 5.5: Extract /home/*.ppm screenshots from disk.img and convert to PNG via /usr/bin/sips
        screenshot_dir = self.repo_dir / "screenshots"
        screenshot_dir.mkdir(parents=True, exist_ok=True)
        apps = ["soffice", "swriter_menu", "scalc", "simpress", "sdraw", "smath", "sbase"]
        disk_img = self.repo_dir / "disk.img"
        for app in apps:
            ppm_dest = screenshot_dir / f"{app}.ppm"
            png_dest = screenshot_dir / f"{app}.png"
            if ppm_dest.exists():
                try:
                    ppm_dest.unlink()
                except OSError:
                    pass
            subprocess.run(["mcopy", "-i", str(disk_img), f"::/home/{app}.ppm", str(ppm_dest)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if ppm_dest.exists() and ppm_dest.stat().st_size > 1000:
                subprocess.run(["/usr/bin/sips", "-s", "format", "png", str(ppm_dest), "--out", str(png_dest)],
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    ppm_dest.unlink()
                except OSError:
                    pass
                if png_dest.exists():
                    self.log(f"Generated screenshot: {png_dest} ({png_dest.stat().st_size} bytes)")

        # Step 6: Verdict & Result Synthesis
        total_time = qemu_execution_time
        self.log(f"Test run finished in {total_time:.2f} seconds.")

        # Print final status
        print("\n" + "=" * 60)
        print(" LIBREOFFICE E2E TEST SUMMARY")
        print("=" * 60)
        for suite in self.expected_suites:
            status = "PASS" if suite in self.passed_suites else "FAIL/NOT REACHED"
            print(f"  - Application {suite:<10}: {status}")

        print(f"  - Total Suites Passed: {len(self.passed_suites)}/6")
        print(f"  - Execution Time     : {total_time:.2f}s (Watchdog limit: {self.timeout}s)")
        print(f"  - Serial Log         : {self.serial_log_path}")
        print(f"  - Screendump PPM     : {self.ppm_shot_path}")
        print("=" * 60)

        if timed_out:
            print("\n[RESULT] FAILED: Strict 30-second watchdog expired (deadlock or hang).")
            return 1

        if self.fatal_errors:
            print(f"\n[RESULT] FAILED with {len(self.fatal_errors)} fatal defect(s):")
            for desc, detail in self.fatal_errors:
                print(f"  * {desc}: {detail}")
            return 1

        if not all_suites_passed or len(self.passed_suites) < len(self.expected_suites):
            print("\n[RESULT] FAILED: Not all application suites completed successfully.")
            return 1

        print("\n[RESULT] SUCCESS: 100% of LibreOffice E2E test suites PASSED in <30s!")
        return 0


# ---------------------------------------------------------------------------
# CLI Entry Point
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Automated Host E2E Test Runner for LibreOffice on HobbyOS")
    parser.add_argument("--arch", default="arm", choices=["arm", "intel"], help="Target architecture (default: arm)")
    parser.add_argument("--timeout", type=float, default=30.0, help="Watchdog timeout in seconds (default: 30.0)")
    parser.add_argument("--no-build", action="store_true", help="Skip pre-test disk.img build step")
    parser.add_argument("--screendump", default="/tmp/lo_screendump.ppm", help="Path for captured screendump")
    parser.add_argument("--outdir", default="/tmp/hobbyos_libreoffice_test", help="Directory for output logs and artifacts")
    parser.add_argument("--verbose", action="store_true", help="Enable verbose diagnostic output")

    args = parser.parse_args()
    runner = LibreOfficeTestRunner(args)
    sys.exit(runner.run())


if __name__ == "__main__":
    main()
