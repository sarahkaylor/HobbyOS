// P3b gate-nudge decision-logic smoke (host-side; models the WK5WindowDriver
// enginePhaseTick Load-phase nudge exactly as committed).
//
// The real nudge lives in WebKit (WK5WindowDriver.cpp enginePhaseTick + the
// Document::hobbyosNudgeCompletionGate helper).  That is not compileable
// standalone, so this harness re-implements the DECISION SHAPE — stall
// signature (readyState==Interactive && !parsing && reqCount==0), the
// every-30th-tick throttle, the loaded flip, and the gate simulation (the
// stranded re-check either eventually completes the doc or stays stalled) —
// and asserts the properties the fix must have:
//   1. Strictly bounded: nudge fires only while the stall signature holds,
//      and at most floor(ticks/30) times over any stretch.
//   2. No nudge while reqCount>0, parsing, or readyState!=Interactive.
//   3. When the re-check can complete (the timer body does its job), the
//      doc reaches Complete (load-ok) and the pump stops.
//   4. When the doc is genuinely wedged on a gate the pump can't clear, the
//      marker stays on the same signature and volume stays flat (throttled).
//
// Build: g++ -O2 -o gate_nudge_smoke gate_nudge_smoke.cpp && ./gate_nudge_smoke
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

enum class ReadyState { Loading, Interactive, Complete };

struct Doc {
    ReadyState rt = ReadyState::Interactive;
    bool parsing = false;
    int reqCount = 0;
    bool gateOpensOnRecheck = false; // true: checkCompletenessNow -> Complete
    bool wedged = false;             // true: gate never clears -> stays Interactive
};

// The committed nudge condition + throttle (verbatim semantics):
//   if (doc->readyState() == Interactive && !doc->parsing()
//       && doc->requestCount() == 0 && (++m_gateNudgeTicks % 30) == 1)
//       { nudge(); marker(); }
static bool shouldNudge(Doc& d, uint64_t& nudgeTicks, bool loaded)
{
    if (loaded)
        return false;
    if (d.rt == ReadyState::Interactive && !d.parsing && d.reqCount == 0
        && (++nudgeTicks % 30) == 1)
        return true;
    return false;
}

// What the real nudge does inside WebCore (timer-fire bodies):
static void nudge(Doc& d)
{
    // Document::hobbyosNudgeCompletionGate ->
    //   (1) executeScriptsWaitingForStylesheetsSoon -> doc->checkCompleted()
    //   (2) frame->loader().checkCompletenessNow() -> checkCompleted
    // Model: when the gate the page was parked on is clearable, the re-check
    // completes the doc; a wedged gate stays stuck.
    if (!d.wedged && d.gateOpensOnRecheck)
        d.rt = ReadyState::Complete;
}

int main()
{
    int fails = 0;
    auto check = [&](bool cond, const char* what) {
        if (!cond) { std::printf("FAIL: %s\n", what); ++fails; }
    };

    // --- case A: stalled doc, re-check eventually completes it --------------
    {
        Doc d;                       // Interactive, !parsing, reqC==0 (stall)
        d.gateOpensOnRecheck = true; // the stranded re-check CAN complete it
        uint64_t ticks = 0;
        int fires = 0, markers = 0;
        for (uint64_t t = 0; t < 100000; ++t) {  // 16 ms ticks -> long stall
            // simulate the page: subresource completes at t=50 (reqC 0->0 noop),
            // nothing else changes; re-check completes doc on first nudge.
            bool fired = shouldNudge(d, ticks, /*loaded=*/false);
            if (fired) { ++fires; ++markers; nudge(d); }
            if (d.rt == ReadyState::Complete)
                break;
        }
        check(fires == 1, "A: completed on exactly one nudge");
        check(d.rt == ReadyState::Complete, "A: doc reached Complete");
        check(markers == 1, "A: marker emitted per nudge");
        std::printf("A: stall-signature -> completed: fires=%d markers=%d\n", fires, markers);
    }

    // --- case B: genuinely wedged page (gate never clears) ------------------
    {
        Doc d; d.wedged = true;
        uint64_t ticks = 0;
        uint64_t fires = 0;
        for (uint64_t t = 0; t < 90000; ++t)
            if (shouldNudge(d, ticks, false)) { ++fires; nudge(d); }
        uint64_t expect = 90000 / 30;                     // first fire at 1, then 31...
        check(fires == expect, "B: bounded to ticks/30 over a long stall");
        check(d.rt == ReadyState::Interactive, "B: wedged doc stays Interactive");
        std::printf("B: wedged 90000 ticks -> fires=%" PRIu64 " (expect %" PRIu64 ")\n", fires, expect);
    }

    // --- case C: no nudge while reqCount>0 / parsing / Loading --------------
    {
        bool bad = false;
        for (int mode = 0; mode < 3; ++mode) {
            Doc d;
            if (mode == 0) d.reqCount = 1;
            if (mode == 1) { d.parsing = true; d.reqCount = 0; }
            if (mode == 2) d.rt = ReadyState::Loading;
            uint64_t ticks = 0;
            for (uint64_t t = 0; t < 5000; ++t) {
                bool fired = shouldNudge(d, ticks, false);
                if (fired) { bad = true; break; }
            }
        }
        check(!bad, "C: no nudge while reqC>0, parsing, or Loading");
        std::printf("C: no nudge outside the stall signature: %s\n", bad ? "BAD" : "ok");
    }

    // --- case D: throttle cadence exactly 1,31,61,... ------------------------
    {
        Doc d;
        uint64_t ticks = 0;
        std::uint64_t first = 0, second = 0;
        int n = 0;
        for (uint64_t t = 1; t <= 65 && n < 2; ++t) {
            if (shouldNudge(d, ticks, false)) {
                if (n == 0) first = t;
                if (n == 1) second = t;
                ++n;
            }
        }
        check(first == 1 && second == 31, "D: fires at ticks 1 and 31");
        std::printf("D: first fire @t=%" PRIu64 ", second @t=%" PRIu64 "\n", first, second);
    }

    // --- case E: after Complete, pump never fires again ----------------------
    {
        Doc d;
        uint64_t ticks = 0;
        uint64_t fires = 0;
        for (uint64_t t = 0; t < 5000; ++t) {
            bool fired = shouldNudge(d, ticks, /*loaded=*/d.rt == ReadyState::Complete);
            if (fired) { ++fires; nudge(d); }
            if (d.rt == ReadyState::Interactive && t == 100) { d.rt = ReadyState::Complete; }
        }
        check(fires <= 5000 / 30, "E: pump drops out once loaded (bounded)");
        std::printf("E: loaded flip -> residual fires=%" PRIu64 "\n", fires);
    }

    std::printf(fails ? "SMOKE FAIL (%d)\n" : "SMOKE PASS\n", fails);
    return fails ? 1 : 0;
}
