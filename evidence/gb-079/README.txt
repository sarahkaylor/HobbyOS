GB-079 — the row-name check gives two verdicts for one tree (PLAN-010, 2026-10-23T03:07:38Z, origin/main 3621f1c)

mode=emitted  (obj/t4_legA.out present, via 'make t4_device_check'): device rows 39 == golden 39 -> PASS
mode=source   (T4_LEGA_OUT=/nonexistent/legA.out):      device rows 41 -> FAIL on pipe_write, sock_write

Both runs are below verbatim. Nothing else differs: same commit, same probes, same golden.

--- mode=emitted ---
=== t4_device --check: row-name set parity (GB-073) ===
  golden rows: 39
  device probe rows: 39
  PASS row: write3 (device+host)
  PASS row: uname_rc (device+host)
  PASS row: uname_sysname_linux (device+host)
  PASS row: uname_machine_x86_64 (device+host)
  PASS row: getrandom_16 (device+host)
  PASS row: urandom_open (device+host)
  PASS row: urandom_read16 (device+host)
  PASS row: null_open_w (device+host)
  PASS row: null_write3 (device+host)
  PASS row: null_open_r (device+host)
  PASS row: null_read1 (device+host)
  PASS row: pipe2 (device+host)
  PASS row: poll_ready (device+host)
  PASS row: poll_ready_revents (device+host)
  PASS row: select_ready (device+host)
  PASS row: select_ready_fdset (device+host)
  PASS row: pselect6_ready (device+host)
  PASS row: pselect6_ready_fdset (device+host)
  PASS row: poll_badfd (device+host)
  PASS row: poll_badfd_revents (device+host)
  PASS row: sockpair (device+host)
  PASS row: sockpoll_ready (device+host)
  PASS row: sockpoll_revents (device+host)
  PASS row: sockselect_ready (device+host)
  PASS row: sockselect_fdset (device+host)
  PASS row: sockpselect_ready (device+host)
  PASS row: sockpselect_fdset (device+host)
  PASS row: fcntl_getfl_rdwr (device+host)
  PASS row: fcntl_setfl_append (device+host)
  PASS row: fcntl_getfl_after (device+host)
  PASS row: fcntl_append_bit (device+host)
  PASS row: ioctl_tiocgwinsz_entty (device+host)
  PASS row: ioctl_tcgets_entty (device+host)
  PASS row: dent_created (device+host)
  PASS row: dent_opendir (device+host)
  PASS row: dent_total (device+host)
  PASS row: dent_have_alpha (device+host)
  PASS row: dent_have_beta (device+host)
  PASS row: dent_have_charlie (device+host)
t4_device --check: PASS — device probe carries exactly the golden row-name set, and the host model contains every golden row.

--- mode=source ---
=== t4_device --check: row-name set parity (GB-073) ===
  fail: leg A ('/nonexistent/legA.out') absent — will skip emitted-row re-check (gate still checks source)
  golden rows: 39
  device probe rows: 41
  PASS row: write3 (device+host)
  PASS row: uname_rc (device+host)
  PASS row: uname_sysname_linux (device+host)
  PASS row: uname_machine_x86_64 (device+host)
  PASS row: getrandom_16 (device+host)
  PASS row: urandom_open (device+host)
  PASS row: urandom_read16 (device+host)
  PASS row: null_open_w (device+host)
  PASS row: null_write3 (device+host)
  PASS row: null_open_r (device+host)
  PASS row: null_read1 (device+host)
  PASS row: pipe2 (device+host)
  PASS row: poll_ready (device+host)
  PASS row: poll_ready_revents (device+host)
  PASS row: select_ready (device+host)
  PASS row: select_ready_fdset (device+host)
  PASS row: pselect6_ready (device+host)
  PASS row: pselect6_ready_fdset (device+host)
  PASS row: poll_badfd (device+host)
  PASS row: poll_badfd_revents (device+host)
  PASS row: sockpair (device+host)
  PASS row: sockpoll_ready (device+host)
  PASS row: sockpoll_revents (device+host)
  PASS row: sockselect_ready (device+host)
  PASS row: sockselect_fdset (device+host)
  PASS row: sockpselect_ready (device+host)
  PASS row: sockpselect_fdset (device+host)
  PASS row: fcntl_getfl_rdwr (device+host)
  PASS row: fcntl_setfl_append (device+host)
  PASS row: fcntl_getfl_after (device+host)
  PASS row: fcntl_append_bit (device+host)
  PASS row: ioctl_tiocgwinsz_entty (device+host)
  PASS row: ioctl_tcgets_entty (device+host)
  PASS row: dent_created (device+host)
  PASS row: dent_opendir (device+host)
  PASS row: dent_total (device+host)
  PASS row: dent_have_alpha (device+host)
  PASS row: dent_have_beta (device+host)
  PASS row: dent_have_charlie (device+host)
  FAIL: device probe emits extra row 'pipe_write' not in golden
  FAIL: device probe emits extra row 'sock_write' not in golden
t4_device --check: FAIL
