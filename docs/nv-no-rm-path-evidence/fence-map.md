pre-init dump: sweep=2048 named=393 | post-init dump: sweep=2048 named=393

| window | range | pre-init | post-init (GSP-RM running) |
|---|---|---|---|
| PMC (boot/intr/enable) | 0x000000..0x000fff | data=4, unreadable-by-design=2 | data=4, unreadable-by-design=2 |
| PBUS 0x009400 | 0x009400..0x0097ff | data=2 | data=2 |
| PDISP 0x100000 | 0x100000..0x100fff | unreadable-by-design=2 | unreadable-by-design=2 |
| GSP falcon 0x110000 | 0x110000..0x1107ff | absent=6, data=70, fenced=436 | absent=38, data=262, exists-not-readable=28, unreadable-by-design=184 |
| GSP fenced tail | 0x110800..0x110fff | absent=65, data=96, exists-not-readable=1, fenced=192, unreadable-by-design=158 | absent=74, data=125, exists-not-readable=1, unreadable-by-design=312 |
| PRI 0x111000 (RISC-V) | 0x111000..0x111fff | fenced=1024 | absent=14, data=111, exists-not-readable=79, unreadable-by-design=820 |
| PFB/WPR2 0x1FA800 | 0x1fa800..0x1fa83f | data=14, unreadable-by-design=2 | data=14, unreadable-by-design=2 |
| SEC2 0x840000 | 0x840000..0x8407ff | data=1, fenced=16 | data=6, fenced=11 |
