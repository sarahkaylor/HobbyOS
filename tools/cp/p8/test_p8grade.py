#!/usr/bin/env python3
"""tools/cp/p8/test_p8grade.py -- unit test of the P8/P9 game-boot verdicts
(tools/cp/p8/p8grade.py; lane H2-early, docs/cp-early.md).

    python3 tools/cp/p8/test_p8grade.py

The serial snippets are short synthetic rows in the shape the launcher, the
kernel and Wine print (no game content).  Before H2 the game phase graded the
first case "launcher-done" with rc 0 (integ12's p8-lvp.2, integ13's lvp1).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p8grade as G  # noqa: E402

PRE = ("[P8] p8launch (C8-game) phase 2\n"
       "[P8] mount 10.0.2.2:/srv/nfs/cp-game -> /GAME ok\n"
       "[P8] warm-up: display driver loaded\n")
LAUNCH = ("[P8] begin game : wine Z:\\GAME\\bin\\x64\\Cyberpunk2077.exe --launcher-skip\n"
          "[P8] game pid=126\n")
REPORTER = ('0148:trace:loaddll:build_module Loaded L"Z:\\\\GAME\\\\bin\\\\x64\\\\REDEngineErrorReporter.exe"'
            " at 0000000140000000: native\n"
            "[KERNEL] Process 131: REDEngineErrorR exited unexpectedly or gracefully.\n")
STEAM = '0140:trace:loaddll:build_module Loaded L"Z:\\\\GAME\\\\bin\\\\x64\\\\steam_api64.dll" at 00007FE375460000: native\n'
TAIL = ("[P8] begin kill : wineserver -k\n[P8] end kill status=0 ms=170\n"
        "[P8] sync done\n[P8] done\n")


class Grade(unittest.TestCase):
    def test_early_exit_after_reporter_is_a_fail(self):
        s = PRE + LAUNCH + REPORTER + "[P8] end game status=0 ms=2886\n" + TAIL
        v, d = G.launcher_end_verdict(s, "game")
        self.assertEqual(v, "early-exit")
        self.assertIn("REDEngineErrorReporter ran: yes", d)
        self.assertIn("2886 ms", d)
        self.assertEqual(G.rc_of(v), 3)
        # the watcher sees the end row before "[P8] done" arrives
        self.assertEqual(G.before_menu_exit(PRE + LAUNCH + REPORTER + "[P8] end game status=0 ms=2886\n")[0],
                         "early-exit")

    def test_early_exit_without_reporter(self):
        s = PRE + LAUNCH + "[P8] end game status=53 ms=565\n" + TAIL
        v, d = G.launcher_end_verdict(s, "game")
        self.assertEqual(v, "early-exit")
        self.assertIn("REDEngineErrorReporter ran: no", d)

    def test_late_exit_with_steam_still_before_menu(self):
        s = PRE + LAUNCH + REPORTER + STEAM + "[P8] end game status=5 ms=45594\n" + TAIL
        v, d = G.launcher_end_verdict(s, "game")
        self.assertEqual(v, "early-exit")
        self.assertIn("steam_api64 loaded", d)

    def test_game_timeout(self):
        s = PRE + LAUNCH + REPORTER + "[P8] timeout game after 3600s\n[P8] end game status=timeout ms=3600012\n" + TAIL
        self.assertEqual(G.launcher_end_verdict(s, "game")[0], "game-timeout")
        self.assertEqual(G.rc_of("game-timeout"), 1)

    def test_mount_failure_is_not_launcher_done(self):
        s = "[P8] p8launch (C8-game) phase 2\n[P8] mount 10.0.2.2:/srv/nfs/cp-game -> /GAME FAILED errno=2\n[P8] done\n"
        v, _ = G.launcher_end_verdict(s, "game")
        self.assertEqual(v, "launcher-ended-before-game")
        self.assertNotEqual(G.rc_of(v), 0)

    def test_pfx_phase_launcher_done_passes(self):
        s = "[P8] p8launch (C8-game) phase 1\n[P8] end wineboot status=0 ms=30000\n[P8] sync done\n[P8] done\n"
        v, _ = G.launcher_end_verdict(s, "pfx")
        self.assertEqual(v, "launcher-done")
        self.assertEqual(G.rc_of(v), 0)

    def test_running_game_has_no_verdict_yet(self):
        self.assertIsNone(G.before_menu_exit(PRE + LAUNCH + REPORTER + STEAM))

    def test_repro_loop(self):
        s = (PRE + "[P8] begin launch 1/2 : wine Z:\\GAME\\bin\\x64\\Cyberpunk2077.exe --launcher-skip chdir rc=0\n"
             + REPORTER + "[P8] end launch 1 status=0 ms=2600 -> early-exit\n"
             + "[P8] begin launch 2/2 : wine x chdir rc=0\n" + REPORTER + STEAM
             + "[P8] end launch 2 alive at 45 s -> survived\n[P8] repro early=1 n=2\n" + TAIL)
        v, d = G.launcher_end_verdict(s, "game")
        self.assertEqual(v, "repro")
        self.assertIn("early=1 n=2", d)
        self.assertIn("1:early-exit@2600+rep", d)
        self.assertIn("2:survived+rep+steam", d)

    def test_replay(self):
        early = PRE + LAUNCH + REPORTER + "[P8] end game status=0 ms=2579\n" + TAIL
        self.assertEqual(G.replay(early)[0], "early-exit")
        menu = PRE + LAUNCH + REPORTER + STEAM
        tl = "+  300.0 s  01:55:00  main menu OCR at s012-t000280: ['NEW GAME', 'LOAD GAME']\n"
        self.assertEqual(G.replay(menu, tl)[0], "main-menu")
        self.assertEqual(G.replay(menu)[0], "running")


if __name__ == "__main__":
    unittest.main(verbosity=2)
