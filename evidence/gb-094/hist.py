#!/usr/bin/env python3
"""GB-094-V: leading-tag histogram over two-record lines.

A "record tag" is a LETTER-LED bracketed identifier ([KERNEL], [ARRIVED], ...).
A digit-led bracket ([0], [1]) is an array subscript, not a tag (GB-092 rule),
so it is ignored here — v1's counter counts it, this classification does not.
"""
import re, sys, collections

TAG = re.compile(r'\[([A-Z][A-Z0-9_.:+-]{1,40})\]')

KERNEL_TAGS = {
    'KERNEL', 'ARRIVED', 'WAVE-TRY', 'WAVE-MANIFEST', 'ALIVE-SET', 'DEBUG',
    'ARP', 'WAITPID', 'HALT-CD', 'EXITPC', 'LOSTWAKE', 'IDLESTUCK', 'ENTRY-KBUF',
    'MEMFAULT', 'BLK-GUARD', 'SMP', 'IRQ', 'VFS', 'NET',
}

def main(path):
    hist = collections.Counter()
    user_led = []
    two_record = 0
    with open(path, 'r', errors='replace') as f:
        for ln, line in enumerate(f, 1):
            tags = [m.group(1) for m in TAG.finditer(line)]
            if len(tags) < 2:
                continue
            two_record += 1
            lead = tags[0]
            hist[lead] += 1
            if lead not in KERNEL_TAGS:
                user_led.append((ln, lead, line.strip()[:160]))
    print(f"two-record lines: {two_record}")
    print("leading tag histogram:")
    for tag, n in hist.most_common():
        kind = "kernel" if tag in KERNEL_TAGS else "USER-PROGRAM"
        print(f"  {n:4d}  [{tag}]  ({kind})")
    print(f"\nlines led by a user-program tag: {len(user_led)}")
    for ln, tag, text in user_led[:20]:
        print(f"  L{ln} [{tag}] {text}")
    return 1 if user_led else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
