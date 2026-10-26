#!/usr/bin/env python3
"""resume_guard_test.py - GB-104 host (T1) self-test.

A resumed agent session is handed its last working directory; if that worktree
was cleaned up between sessions, every shell call fails `cd` -- one error per
command.  GB-104's fix is a guard that reports the dead cwd EXACTLY ONCE naming
the path, a stale-worktree pruner so a resume is never pointed at a dead dir,
and this test proving both (and that a normal resume with an existing cwd
succeeds).  A non-existing-cwd resume MUST NOT be reported per-command.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GUARD = os.path.join(HERE, "resume-cwd-guard.sh")
PRUNE = os.path.join(HERE, "prune-stale-worktrees.sh")

fails = 0
def check(cond, msg):
    global fails
    if cond:
        print("  PASS:", msg)
    else:
        fails += 1
        print("  FAIL:", msg)

def run(*a, **kw):
    return subprocess.run(*a, capture_output=True, text=True, **kw)

# --- 1. normal resume: existing cwd -> exit 0, no RESUME-CWD-GUARD error -----
with tempfile.TemporaryDirectory() as td:
    r = run(["bash", GUARD, td])
    check(r.returncode == 0, "existing cwd -> guard exit 0 (normal resume succeeds)")
    check("RESUME-CWD-GUARD" not in (r.stdout + r.stderr),
          "existing cwd -> no startup error line")

# --- 2. resume into a DELETED cwd -> exactly ONE error naming the path -------
dead = os.path.join(tempfile.gettempdir(), "gb104_resume_dead_cwd_%d" % os.getpid())
r = run(["bash", GUARD, dead])   # never created -> does not exist
err = (r.stdout + r.stderr)
guard_lines = [l for l in err.splitlines() if "RESUME-CWD-GUARD" in l]
check(r.returncode != 0, "deleted cwd -> guard exits non-zero")
check(len(guard_lines) == 1, "deleted cwd -> EXACTLY ONE startup error (got %d)" % len(guard_lines))
check(dead in " ".join(guard_lines), "the single error NAMES the path (not generic)")

# --- 3. stale worktrees are pruned; a live worktree is kept ------------------
with tempfile.TemporaryDirectory() as repo:
    repo = os.path.realpath(repo)
    run(["git", "-C", repo, "init", "-q"])
    run(["git", "-C", repo, "config", "user.email", "t@t"])
    run(["git", "-C", repo, "config", "user.name", "t"])
    with open(os.path.join(repo, "f"), "w") as f: f.write("x")
    run(["git", "-C", repo, "add", "f"])
    run(["git", "-C", repo, "commit", "-qm", "init"])
    wtroot = os.path.join(repo, ".worktrees"); os.makedirs(wtroot, exist_ok=True)
    live = os.path.join(wtroot, "wt_live")
    stale = os.path.join(wtroot, "wt_stale")
    run(["git", "-C", repo, "worktree", "add", "--detach", live])
    # make `stale` an orphan: add it, then delete its .git metadata and the entry
    run(["git", "-C", repo, "worktree", "add", "--detach", stale])
    # remove the metadata so git no longer lists wt_stale, leaving the dir behind
    import shutil
    shutil.rmtree(os.path.join(repo, ".git", "worktrees", "wt_stale"), ignore_errors=True)
    empty = os.path.join(wtroot, "wt_empty"); os.makedirs(empty)  # truly-empty leftover
    pre_live = "wt_live" in run(["git", "-C", repo, "worktree", "list"]).stdout
    pr = run(["bash", PRUNE, repo])
    check(pr.returncode == 0, "prune exits 0")
    check("PRUNE-STALE-WORKTREES" in pr.stdout, "prune report line present")
    check(os.path.isdir(live), "live/git-listed worktree kept")
    check(os.path.isdir(stale) and "REFUSED" in pr.stdout,
          "stale dir with uncommitted/unknown work REFUSED, NOT deleted (GB-104 RED)")
    check(not os.path.isdir(empty), "empty stale leftover dir removed")

# --- verdict ----------------------------------------------------------------
if fails:
    print("RESUME_GUARD_TEST: FAIL (%d failure(s))" % fails)
    sys.exit(1)
print("RESUME_GUARD_TEST: PASS - one startup error for a dead cwd, normal resume succeeds, stale worktrees pruned")
sys.exit(0)
