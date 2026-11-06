# L65 variance: three 1800 s timelines (30 s resolution)

`nfs` is cumulative READ bytes from `[NFSRTT] periodic`; `xsw` is
presents from `[XSW] n=`; `live`/`res` are `[THRS] pid=4`; `xo` is
the last XML the game opened. `post-xml` = NFS still growing with no
new XML (asset/art load); `parked` = neither NFS nor presents moved.
`t` is the launcher's own `child still running after Ns` clock, so a
row is ±10 s even though the sampler runs on a 30 s grid.

## run1

| t | NFS MiB | xsw | live | res MiB | last XO | phase | stalls (retr/blkq/pipe) |
|---:|---:|---:|---:|---:|---|---|---|
| 0 | 0.0 | 0 | 24 | 77 |  | - | -/-/- |
| 30 | 8.2 | 0 | 26 | 463 |  | post-xml | -/-/- |
| 60 | 38.7 | 0 | 46 | 591 | PossibleUnknownThreatMarker.xml | defs | -/-/- |
| 90 | 42.7 | 0 | 34 | 617 | ThingSetMakers_Misc.xml | defs | -/-/- |
| 120 | 43.7 | 70 | 27 | 624 | TraderKinds_Base_Empire.xml | defs | 0/0/0 |
| 150 | 45.7 | 100 | 29 | 631 | WorshippedTerminal.xml | defs | 0/0/0 |
| 180 | 45.7 | 130 | 27 | 632 | WorshippedTerminal.xml | defs | 0/0/0 |
| 210 | 46.7 | 160 | 27 | 667 | Stats_Basics_General.xml | post-xml | 0/0/0 |
| 240 | 46.7 | 180 | 41 | 753 | MainTabs.xml | language | 0/0/1 |
| 270 | 47.7 | 210 | 34 | 755 | Example_Traits.xml | language | 0/0/1 |
| 300 | 47.7 | 230 | 28 | 755 | Example_Traits.xml | language | 0/0/1 |
| 330 | 47.7 | 260 | 44 | 785 | Letters.xml | language | 0/0/5 |
| 360 | 48.7 | 260 | 35 | 818 | Letters.xml | language | 0/0/9 |
| 390 | 52.3 | 260 | 26 | 822 | Letters.xml | language | 0/0/9 |
| 420 | 74.2 | 260 | 26 | 879 | Letters.xml | language | 0/0/9 |
| 450 | 121.5 | 260 | 26 | 937 | Letters.xml | language | 0/0/9 |
| 480 | 180.7 | 260 | 26 | 1003 | Letters.xml | language | 0/0/9 |
| 510 | 252.7 | 260 | 26 | 1126 | Letters.xml | language | 0/0/9 |
| 540 | 264.7 | 260 | 26 | 1166 | Letters.xml | language | 0/0/9 |
| 570 | 264.7 | 260 | 26 | 1184 | Letters.xml | language | 0/0/9 |
| 600 | 264.7 | 260 | 26 | 1202 | Letters.xml | language | 0/0/9 |
| 630 | 264.7 | 260 | 26 | 1221 | Letters.xml | language | 0/0/9 |
| 660 | 264.7 | 260 | 26 | 1239 | Letters.xml | language | 0/0/9 |
| 690 | 264.7 | 260 | 26 | 1257 | Letters.xml | language | 0/0/9 |
| 720 | 264.7 | 260 | 26 | 1275 | Letters.xml | language | 0/0/9 |
| 750 | 264.7 | 260 | 26 | 1293 | Letters.xml | language | 0/0/9 |
| 780 | 264.7 | 260 | 26 | 1311 | Letters.xml | language | 0/0/9 |
| 810 | 264.7 | 260 | 26 | 1329 | Letters.xml | language | 0/0/9 |
| 840 | 264.7 | 260 | 26 | 1348 | Letters.xml | language | 0/0/9 |
| 870 | 264.7 | 260 | 26 | 1366 | Letters.xml | language | 0/0/9 |
| 900 | 264.7 | 260 | 26 | 1385 | Letters.xml | language | 0/0/9 |
| 930 | 264.7 | 260 | 26 | 1403 | Letters.xml | language | 0/0/9 |
| 960 | 264.7 | 260 | 26 | 1440 | Letters.xml | language | 0/0/9 |
| 990 | 264.7 | 260 | 26 | 1458 | Letters.xml | language | 0/0/9 |
| 1020 | 264.7 | 260 | 26 | 1476 | Letters.xml | language | 0/0/9 |
| 1050 | 264.7 | 260 | 26 | 1495 | Letters.xml | language | 0/0/9 |
| 1080 | 264.7 | 260 | 26 | 1513 | Letters.xml | language | 0/0/9 |
| 1110 | 264.7 | 260 | 26 | 1531 | Letters.xml | language | 0/0/9 |
| 1140 | 264.7 | 260 | 26 | 1549 | Letters.xml | language | 0/0/9 |
| 1170 | 264.7 | 260 | 26 | 1568 | Letters.xml | language | 0/0/9 |
| 1200 | 264.7 | 260 | 25 | 1586 | Letters.xml | language | 0/0/9 |
| 1230 | 264.7 | 260 | 25 | 1604 | Letters.xml | language | 0/0/9 |
| 1260 | 264.7 | 260 | 25 | 1624 | Letters.xml | language | 0/0/9 |
| 1290 | 264.7 | 260 | 25 | 1642 | Letters.xml | language | 0/0/9 |
| 1320 | 264.7 | 260 | 25 | 1661 | Letters.xml | language | 0/0/9 |
| 1350 | 264.7 | 260 | 25 | 1679 | Letters.xml | language | 0/0/9 |
| 1380 | 264.7 | 260 | 25 | 1697 | Letters.xml | language | 0/0/9 |
| 1410 | 264.7 | 260 | 25 | 1734 | Letters.xml | language | 0/0/9 |
| 1440 | 264.7 | 260 | 25 | 1752 | Letters.xml | language | 0/0/9 |
| 1470 | 264.7 | 260 | 25 | 1771 | Letters.xml | language | 0/0/9 |
| 1500 | 264.7 | 260 | 25 | 1789 | Letters.xml | language | 0/0/9 |
| 1530 | 264.7 | 260 | 25 | 1807 | Letters.xml | language | 0/0/9 |
| 1560 | 264.7 | 260 | 25 | 1825 | Letters.xml | language | 0/0/9 |
| 1590 | 264.7 | 260 | 25 | 1844 | Letters.xml | language | 0/0/9 |
| 1620 | 264.7 | 260 | 25 | 1862 | Letters.xml | language | 0/0/9 |
| 1650 | 264.7 | 260 | 25 | 1880 | Letters.xml | language | 0/0/9 |
| 1680 | 264.7 | 260 | 25 | 1899 | Letters.xml | language | 0/0/9 |

## run2

| t | NFS MiB | xsw | live | res MiB | last XO | phase | stalls (retr/blkq/pipe) |
|---:|---:|---:|---:|---:|---|---|---|
| 0 | 0.0 | 0 | 25 | 210 |  | - | 0/0/0 |
| 30 | 37.7 | 3 | 43 | 589 | AmbushHidden.xml | defs | 0/0/0 |
| 60 | 42.7 | 30 | 38 | 617 | Cultures.xml | defs | 0/0/0 |
| 90 | 43.7 | 60 | 27 | 623 | TraderKinds_Base_Empire.xml | defs | 0/0/0 |
| 120 | 45.7 | 90 | 29 | 630 | Precepts_WorkDrive.xml | defs | 0/0/0 |
| 150 | 45.7 | 110 | 27 | 630 | WorshippedTerminal.xml | defs | 0/0/0 |
| 180 | 46.7 | 140 | 27 | 637 | UI_Oneshots_Designation.xml | defs | 0/0/0 |
| 210 | 46.7 | 170 | 27 | 750 | LanguageInfo.xml | language | 0/0/0 |
| 240 | 47.7 | 200 | 39 | 751 | Example_Traits.xml | language | 0/0/2 |
| 270 | 47.7 | 230 | 31 | 752 | Example_Traits.xml | language | 0/0/2 |
| 300 | 47.7 | 260 | 41 | 755 | FloatMenu.xml | language | 0/0/4 |
| 330 | 48.7 | 270 | 41 | 813 | Designators.xml | language | 0/0/8 |
| 360 | 51.3 | 270 | 31 | 815 | Designators.xml | language | 0/0/8 |
| 390 | 60.4 | 270 | 26 | 839 | Designators.xml | language | 0/0/8 |
| 420 | 101.9 | 270 | 26 | 912 | Designators.xml | language | 0/0/8 |
| 450 | 166.5 | 270 | 26 | 985 | Designators.xml | language | 0/0/8 |
| 480 | 264.7 | 270 | 37 | 1136 | Designators.xml | language | 0/1/8 |

## run3

| t | NFS MiB | xsw | live | res MiB | last XO | phase | stalls (retr/blkq/pipe) |
|---:|---:|---:|---:|---:|---|---|---|
| 0 | 0.0 | 0 | 24 | 177 |  | - | 0/0/0 |
| 30 | 23.0 | 1 | 26 | 490 |  | post-xml | 0/0/0 |
| 60 | 42.7 | 20 | 41 | 610 | ThingSetMakers_Misc.xml | defs | 0/0/0 |
| 90 | 43.7 | 40 | 35 | 620 | TraderKinds_Base_Empire.xml | defs | 0/0/0 |
| 120 | 43.7 | 70 | 27 | 623 | TraderKinds_Base_Empire.xml | defs | 0/0/0 |
| 150 | 45.7 | 100 | 29 | 629 | SlaveInteractionMode.xml | defs | 0/0/0 |
| 180 | 45.7 | 130 | 27 | 630 | WorshippedTerminal.xml | defs | 0/0/0 |
| 210 | 46.7 | 160 | 29 | 637 | Traits.xml | post-xml | 0/0/0 |
| 240 | 46.7 | 190 | 27 | 749 | LanguageInfo.xml | language | 0/0/0 |
| 270 | 47.7 | 210 | 40 | 751 | Example_Traits.xml | language | 0/0/2 |
| 300 | 47.7 | 240 | 29 | 751 | Example_Traits.xml | language | 0/0/2 |
| 330 | 47.7 | 270 | 27 | 752 | GameplayCommands.xml | language | 0/0/3 |
| 360 | 47.7 | 290 | 41 | 808 | Dialogs_Various.xml | language | 0/0/8 |
| 390 | 51.2 | 290 | 32 | 814 | Dialogs_Various.xml | language | 0/0/8 |
| 420 | 64.6 | 290 | 26 | 839 | Dialogs_Various.xml | language | 0/0/8 |
| 450 | 166.4 | 290 | 26 | 972 | Dialogs_Various.xml | language | 0/0/8 |
| 480 | 240.6 | 290 | 26 | 1111 | Dialogs_Various.xml | language | 0/0/8 |
| 510 | 264.6 | 290 | 37 | 1137 | Dialogs_Various.xml | language | 0/1/8 |

