#ifndef SIG_TRACE_H
#define SIG_TRACE_H

/*
 * sig_trace — the per-signal console rows, off by default.
 *
 * C1-thr (docs/cp-thr.md §3).  The M6 signal work left a dozen bounded rows
 * on the tgkill -> park interrupt -> frame plant -> sigreturn path ([SIGIN],
 * [SIG] send, [TGTK], [FWK], [SFS], [SIGRZ], [FPUA], [SIGP], [SFP], [SIGD],
 * [SIGR], [SFR], [RT], [TEXIT], ...).  Each is one synchronous UART record
 * and the UART costs two port exits per byte, so the ~2 KB a single signal
 * printed was the signal's latency: 10-13 ms from tgkill to handler entry on
 * a parked thread (strix, -cpu host), of which the delivery itself is tens
 * of microseconds.  They also landed between a probe's own console writes.
 *
 * SIGTR(n) is the row budget a site may print: n with `make SIG_TRACE=1`
 * (-DSIG_TRACE_CONSOLE), else 0.  Rows that report an ANOMALY (a blocked
 * stop-the-world signal, a skewed stack, a failed plant) are not routed
 * through this and still print.
 */
#ifdef SIG_TRACE_CONSOLE
#define SIGTR(n) (n)
#else
#define SIGTR(n) 0
#endif

#endif /* SIG_TRACE_H */
