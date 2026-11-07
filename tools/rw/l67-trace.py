#!/usr/bin/env python3
"""L67: read a QEMU -trace log of the NVMe device and answer the one question
the kernel row cannot: did the doorbell for the stalled command reach the
device, and what happened to the controller at that moment?

Usage: l67-trace.py <trace-file> [--around N]

The trace is ordered (no timestamps), so command identity is reconstructed by
counting SQ doorbell writes: the Nth doorbell is the Nth command submitted
since boot (the kernel submits one command at a time).  Every controller-state
event (CC write / stop / start / FLR) is reported with the doorbell index it
follows, which is exactly the "which command was in flight" answer.
"""
import sys

def main():
    path = sys.argv[1]
    around = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    events = []          # (kind, payload, doorbell_index_before)
    n_db = 0
    lines = 0
    with open(path, "r", errors="replace") as f:
        for ln in f:
            lines += 1
            ln = ln.strip()
            if not ln:
                continue
            name = ln.split(" ", 1)[0]
            rest = ln[len(name):].strip()
            if name == "pci_nvme_mmio_doorbell_sq":
                n_db += 1
                continue
            if name == "pci_nvme_mmio_doorbell_cq":
                continue
            events.append((name, rest, n_db))
    print(f"trace lines={lines} sq_doorbells={n_db} state_events={len(events)}")
    for name, rest, idx in events:
        print(f"  after {idx:7d} doorbells: {name}: {rest}")
    if events:
        last = events[-1]
        print(f"\nlast state event is after doorbell {last[2]} of {n_db}"
              f" -> {n_db - last[2]} commands submitted since")
    # tail of the doorbell stream
    print("\n(no io_cmd events enabled in this run)" if not any(
        e[0] == "pci_nvme_io_cmd" for e in events) else "")

if __name__ == "__main__":
    main()
