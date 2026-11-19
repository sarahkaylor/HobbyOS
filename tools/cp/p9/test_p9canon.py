#!/usr/bin/env python3
"""tools/cp/p9/test_p9canon.py -- boot_p9's shot normalisation (game_view /
canon_image / focus_in) on synthetic frames (lane H2-early, cp-hunt H1-08).

    python3 tools/cp/p9/test_p9canon.py

A frame is drawn the way the main menu looks to focus_in(): the focused row
is a yellow frame ~36 px tall around a row of the 1280x720 game picture.  A
1920x1080 game draws the same menu 1.5x larger.  Synthetic pixels only (no
game content); the 1.5x scale is the assumption H1-08 rests on and has not
been checked on a real 1080p shot.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image, ImageDraw  # noqa: E402

import boot_p9 as B  # noqa: E402

YELLOW = (230, 190, 60)


def game_frame(label, scale=1.0, size=(1280, 720)):
    """the game's picture with `label`'s row framed (game coordinates =
    canonical - (320, 180), times `scale`)"""
    im = Image.new("RGB", size, (20, 10, 10))
    ry = dict(B.MENUS["main"])[label] - 180
    d = ImageDraw.Draw(im)
    x0, x1 = int(100 * scale), int(460 * scale)
    y0, y1 = int((ry - 18) * scale), int((ry + 18) * scale)
    d.rectangle((x0, y0, x1, y1), outline=YELLOW, width=max(2, int(3 * scale)))
    d.rectangle((x0 + 20, (y0 + y1) // 2 - 4, x0 + 200, (y0 + y1) // 2 + 4), fill=YELLOW)
    return im


def on_screen(game, screen=(1920, 1080), at=(320, 180)):
    s = Image.new("RGB", screen, (40, 40, 60))
    s.paste(game, at)
    return s


class Canon(unittest.TestCase):
    def check(self, shot, xscreen, game_res, want):
        label, yc, rows = B.focus_in(B.canon_image(shot, xscreen, game_res), "main")
        self.assertEqual(label, want, f"rows {rows} centre {yc}")

    def test_720p_game_in_1080p_screen(self):        # ctl1's layout
        self.check(on_screen(game_frame("LOAD GAME")), "1920x1080", "1280x720", "LOAD GAME")

    def test_720p_game_on_720p_screen(self):          # the scanout blob / VNC shot
        self.check(game_frame("SETTINGS"), "1280x720", "1280x720", "SETTINGS")

    def test_720p_game_top_left_of_1080p_glass(self):  # desktop path, 1280x720 X screen
        self.check(on_screen(game_frame("CONTINUE"), at=(0, 0)), "1280x720", "1280x720", "CONTINUE")

    def test_1080p_game_needs_game_res(self):         # H1-08
        shot = game_frame("LOAD GAME", scale=1.5, size=(1920, 1080))
        self.check(shot, "1920x1080", "1920x1080", "LOAD GAME")
        for lab in ("NEW GAME", "QUIT GAME"):
            self.check(game_frame(lab, scale=1.5, size=(1920, 1080)), "1920x1080", "1920x1080", lab)

    def test_1080p_game_without_game_res_misses(self):
        """the pre-H1-08 behaviour: the 1080p shot read as a 720p layout finds
        a wrong row or none (h-fq1: SEEK FAILED)"""
        shot = game_frame("LOAD GAME", scale=1.5, size=(1920, 1080))
        label, _, _ = B.focus_in(B.canon_image(shot, "1920x1080", "1280x720"), "main")
        self.assertNotEqual(label, "LOAD GAME")

    def test_prompt_crop_same_region(self):
        g = game_frame("LOAD GAME")
        a = B.game_view(on_screen(g), "1920x1080").crop((300, 540, 980, 660))
        b = g.crop((300, 540, 980, 660))
        self.assertEqual(a.tobytes(), b.tobytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)
