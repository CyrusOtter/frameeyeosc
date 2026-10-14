// Tests for re-centering by itself when the headset is put on (auto_recenter.cpp): when it arms, when it fires, and
// what keeps it from firing. Built with the panel as auto-recenter-test; exits non-zero on failure.
#include "auto_recenter.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

/**
 * Record a failed check.
 * @param ok the check
 * @param what what was checked
 * @param line where
 */
void check(bool ok, const char* what, int line) {
    if (ok) return;
    ++gFailures;
    std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
}

#define CHECK(condition) check((condition), #condition, __LINE__)

using auto_recenter::Inputs;
using auto_recenter::Watcher;

/** A watcher fed the way the panel's loop feeds it: every 0.25 s, with the state set on `in`. */
struct Run {
    Watcher watcher;
    Inputs in;
    double now = 100.0;
    int starts = 0;                 ///< how many times it said to start
    double startedAt = -1.0;        ///< when it last did
    std::vector<std::string> logs;  ///< every line it logged

    /** A worn headset with a gaze fit, frameeyeosc running, SteamVR connected and the dashboard closed. */
    Run() {
        in.enabled = true;
        in.fitted = true;
        in.running = true;
        in.tracking = true;
        in.vrConnected = true;
    }

    /**
     * Let time pass with the state as it is.
     * @param seconds how long
     */
    void wait(double seconds) {
        const double end = now + seconds;
        while (now < end - 1e-9) {
            const auto_recenter::Step step = watcher.update(now, in);
            if (!step.log.empty()) logs.push_back(step.log);
            if (step.start) {
                ++starts;
                startedAt = now;
                // The fit runs for a few seconds after it starts, as in the panel
                in.fitActive = true;
            }
            now += 0.25;
        }
    }

    /**
     * Whether a logged line contains a text.
     * @param text the text
     * @return how many lines do
     */
    int logged(const char* text) const {
        int count = 0;
        for (const std::string& line : logs) count += line.find(text) != std::string::npos ? 1 : 0;
        return count;
    }

    /** The re-center that started has finished. */
    void finishFit() { in.fitActive = false; }

    /**
     * An eye fit ended, told to the watcher as the panel does when its record ends.
     * @param byItself the watcher started it (not the panel's buttons)
     * @param byDashboard it was stopped because the dashboard opened
     */
    void endFit(bool byItself, bool byDashboard) {
        in.fitActive = false;
        const std::string line = watcher.fitEnded(byItself, byDashboard);
        if (!line.empty()) logs.push_back(line);
    }

    /**
     * SteamVR opens its dashboard a moment after the re-center started, which stops it.
     * @param byItself the watcher started the fit it stopped
     */
    void dashboardStops(bool byItself = true) {
        in.dashboardOpen = true;
        wait(0.25);
        endFit(byItself, true);
    }
};

void testStart() {
    // The first wearing: armed from the start, fires once the eyes have been tracked for 3 s
    Run run;
    run.wait(2.9);
    CHECK(run.starts == 0);
    run.wait(0.5);
    CHECK(run.starts == 1);
    CHECK(run.startedAt >= 103.0 && run.startedAt <= 103.25);
    CHECK(!run.watcher.armed());
    CHECK(run.logged("auto re-center: starting") == 1);
    // Once only
    run.finishFit();
    run.wait(60);
    CHECK(run.starts == 1);
    // Started with the headset off: fires 3 s after it is put on
    Run off;
    off.in.tracking = false;
    off.wait(30);
    CHECK(off.starts == 0 && off.watcher.armed());
    off.in.tracking = true;
    off.wait(2.9);
    CHECK(off.starts == 0);
    off.wait(0.5);
    CHECK(off.starts == 1);
}

void testShortGapIgnored() {
    Run run;
    run.wait(5);
    CHECK(run.starts == 1);
    run.finishFit();
    // Hiccups of 1.2-2.2 s (and anything under 5 s) are not taking the headset off
    for (double gap : {1.25, 2.25, 4.75}) {
        run.in.tracking = false;
        run.wait(gap);
        run.in.tracking = true;
        run.wait(10);
    }
    CHECK(run.starts == 1);
    CHECK(run.logged("put on") == 0);
}

void testLongGapArms() {
    Run run;
    run.wait(5);
    run.finishFit();
    // Taken off for 86 s, put back on
    run.in.tracking = false;
    run.wait(86);
    CHECK(!run.watcher.armed() && run.starts == 1);
    run.in.tracking = true;
    run.wait(0.25);
    CHECK(run.watcher.armed());
    CHECK(run.logged("put on (tracking was off 86.") == 1);
    run.wait(2.5);
    CHECK(run.starts == 1);
    run.wait(1);
    CHECK(run.starts == 2);
    // Exactly the 5 s it takes also counts
    run.finishFit();
    run.in.tracking = false;
    run.wait(auto_recenter::kOffSec);
    run.in.tracking = true;
    run.wait(4);
    CHECK(run.starts == 3);
}

void testSettleRestarts() {
    Run run;
    run.in.tracking = false;
    run.wait(1);
    // On for 2 s, lost for a moment (the headset still being adjusted), then on again: 3 s from the return
    run.in.tracking = true;
    run.wait(2);
    run.in.tracking = false;
    run.wait(0.5);
    CHECK(run.logged("eyes lost") == 1);
    run.in.tracking = true;
    const double back = run.now;
    run.wait(2.9);
    CHECK(run.starts == 0);
    run.wait(0.5);
    CHECK(run.starts == 1 && run.startedAt >= back + auto_recenter::kSettleSec);
}

void testDashboardOpenBlocks() {
    Run run;
    run.in.dashboardOpen = true;
    run.wait(20);
    CHECK(run.starts == 0 && run.watcher.armed());
    // Closed: waits a second more
    run.in.dashboardOpen = false;
    run.wait(0.75);
    CHECK(run.starts == 0);
    run.wait(0.5);
    CHECK(run.starts == 1);
    // Opened for a moment just before it would fire: the second starts over
    Run blink;
    blink.wait(2.5);
    blink.in.dashboardOpen = true;
    blink.wait(0.25);
    blink.in.dashboardOpen = false;
    blink.wait(0.75);
    CHECK(blink.starts == 0);
    blink.wait(0.5);
    CHECK(blink.starts == 1);
}

void testNotConnectedWaits() {
    // Not connected to SteamVR yet (the Frame starting up): the dashboard reads closed, but it can't be known, so no dot
    // however long the eyes have been tracked
    Run run;
    run.in.vrConnected = false;
    run.wait(30);
    CHECK(run.starts == 0 && run.watcher.armed());
    // Connected: the dashboard's second closed counts from now
    run.in.vrConnected = true;
    const double connected = run.now;
    run.wait(0.75);
    CHECK(run.starts == 0);
    run.wait(0.5);
    CHECK(run.starts == 1 && run.startedAt >= connected + auto_recenter::kClosedSec &&
          run.startedAt <= connected + auto_recenter::kClosedSec + 0.25);
    // ...and SteamVR opened its dashboard right after: the stopped try is tried again once it has been closed a second
    run.dashboardStops();
    CHECK(run.watcher.armed());
    CHECK(run.logged("auto re-center: stopped by the dashboard opening; trying again once it has been closed 1 s "
                     "(retry 1 of 2)") == 1);
    run.wait(20);
    CHECK(run.starts == 1);
    run.in.dashboardOpen = false;
    const double closed = run.now;
    run.wait(0.75);
    CHECK(run.starts == 1);
    run.wait(0.5);
    CHECK(run.starts == 2 && run.startedAt >= closed + auto_recenter::kClosedSec);
    CHECK(run.logged("auto re-center: starting (retry 1 of 2)") == 1);
    // That one went through: nothing more this wearing
    run.endFit(true, false);
    run.wait(60);
    CHECK(run.starts == 2 && !run.watcher.armed());
}

void testDashboardRetries() {
    // Put on after the Frame slept 12 minutes (2026-10-12 23:38): the re-center started, and SteamVR opened its
    // dashboard a moment later as it woke up
    Run run;
    run.wait(5);
    run.finishFit();
    run.in.tracking = false;
    run.wait(3);
    run.now += 721;
    run.in.tracking = true;
    run.wait(3.5);
    CHECK(run.starts == 2);
    CHECK(run.watcher.willRetry(true, true));
    run.dashboardStops();
    CHECK(run.watcher.armed());
    // The settle time doesn't start over (the eyes are still on): only the dashboard's second
    run.in.dashboardOpen = false;
    run.wait(1.25);
    CHECK(run.starts == 3 && run.logged("starting (retry 1 of 2)") == 1);
    // Stopped by the dashboard again: one more try
    run.dashboardStops();
    CHECK(run.watcher.armed() && run.logged("(retry 2 of 2)") == 1);
    run.in.dashboardOpen = false;
    run.wait(1.25);
    CHECK(run.starts == 4 && run.logged("starting (retry 2 of 2)") == 1);
    // ...and again: that's all for this wearing (someone opening it on purpose isn't asked over and over)
    CHECK(!run.watcher.willRetry(true, true));
    run.dashboardStops();
    CHECK(!run.watcher.armed());
    CHECK(run.logged("auto re-center: stopped by the dashboard opening; not trying again (2 retries used this "
                     "wearing)") == 1);
    run.in.dashboardOpen = false;
    run.wait(60);
    CHECK(run.starts == 4);
    // Put on again: a new wearing, with its own tries
    run.in.tracking = false;
    run.wait(20);
    run.in.tracking = true;
    run.wait(4);
    CHECK(run.starts == 5 && run.logs.back() == "auto re-center: starting");
    CHECK(run.watcher.willRetry(true, true));
    run.dashboardStops();
    CHECK(run.watcher.armed() && run.logged("(retry 1 of 2)") == 3);
    run.in.dashboardOpen = false;
    run.wait(1.25);
    CHECK(run.starts == 6);
    // A retry waits like the first try: for the eyes to be back and settled
    run.dashboardStops();
    run.in.dashboardOpen = false;
    run.in.tracking = false;
    run.wait(1);
    run.in.tracking = true;
    run.wait(2.5);
    CHECK(run.starts == 6);
    run.wait(1);
    CHECK(run.starts == 7 && run.logged("starting (retry 2 of 2)") == 2);
}

void testOtherEndsNoRetry() {
    // Any other end of the re-center it started (unsteady, no answer, done, stopped by hand...): one try per wearing
    Run run;
    run.wait(3.5);
    CHECK(run.starts == 1);
    CHECK(!run.watcher.willRetry(true, false));
    const size_t lines = run.logs.size();
    run.endFit(true, false);
    CHECK(run.logs.size() == lines && !run.watcher.armed());
    run.wait(60);
    CHECK(run.starts == 1);
}

void testPanelFitUnaffected() {
    // A fit started from the panel and stopped by the dashboard opening: the user's, never tried again by itself
    Run run;
    run.wait(3.5);
    run.endFit(true, false);
    run.in.fitActive = true;
    run.wait(5);
    CHECK(!run.watcher.willRetry(false, true));
    const size_t lines = run.logs.size();
    run.dashboardStops(false);
    CHECK(run.logs.size() == lines && !run.watcher.armed());
    run.in.dashboardOpen = false;
    run.wait(60);
    CHECK(run.starts == 1);
    // ...also while a retry was waiting: the panel's fit takes over this wearing, and nothing is tried after it
    Run waiting;
    waiting.wait(3.5);
    waiting.dashboardStops();
    CHECK(waiting.watcher.armed());
    waiting.in.fitActive = true;  // pressed in the panel, the dashboard still open
    waiting.wait(5);
    CHECK(!waiting.watcher.armed() && waiting.logged("skipped (an eye fit was started)") == 1);
    waiting.in.dashboardOpen = false;
    waiting.wait(1);
    waiting.dashboardStops(false);
    waiting.in.dashboardOpen = false;
    waiting.wait(60);
    CHECK(waiting.starts == 1 && !waiting.watcher.armed());
}

void testNotRunningFreezes() {
    // frameeyeosc restarted (an update) while worn: no dot, however long it took
    Run run;
    run.wait(5);
    run.finishFit();
    run.in.running = false;
    run.in.tracking = false;
    run.wait(40);
    run.in.running = true;
    run.in.tracking = false;  // starting up: no samples yet for a moment
    run.wait(1);
    run.in.tracking = true;
    run.wait(10);
    CHECK(run.starts == 1);
    CHECK(run.logged("put on") == 0);
    // Armed but frameeyeosc stopped: nothing fires, and the settle time doesn't run meanwhile
    Run armed;
    armed.in.running = false;
    armed.wait(30);
    CHECK(armed.starts == 0 && armed.watcher.armed());
    armed.in.running = true;
    armed.wait(2.9);
    CHECK(armed.starts == 0);
    armed.wait(0.5);
    CHECK(armed.starts == 1);
    // The off time only counts while it runs: 3 s off, 60 s stopped, 1 s off again is not a put-on
    Run split;
    split.wait(5);
    split.finishFit();
    split.in.tracking = false;
    split.wait(3);
    split.in.running = false;
    split.wait(60);
    split.in.running = true;
    split.wait(1);
    split.in.tracking = true;
    split.wait(10);
    CHECK(split.starts == 1);
    // ...but 3 + 3 s is
    split.in.tracking = false;
    split.wait(3);
    split.in.running = false;
    split.wait(60);
    split.in.running = true;
    split.wait(3);
    split.in.tracking = true;
    split.wait(4);
    CHECK(split.starts == 2);
}

void testSuspendCountsAsOff() {
    // Taken off, and the Frame slept a minute (no updates at all; the clock keeps running): put on again, it arms.
    // The first status after waking up is the stale one from before, read as "not running"
    Run stale;
    stale.wait(5);
    stale.finishFit();
    stale.in.tracking = false;
    stale.wait(3);
    stale.now += 60;
    stale.in.running = false;
    stale.wait(0.25);
    stale.in.running = true;
    stale.in.tracking = true;
    stale.wait(0.25);
    CHECK(stale.watcher.armed() && stale.logged("put on (tracking was off 63.") == 1);
    stale.wait(4);
    CHECK(stale.starts == 2);
    // ...or the status from before the sleep had not been read again yet (still running, tracking off)
    Run cached;
    cached.wait(5);
    cached.finishFit();
    cached.in.tracking = false;
    cached.wait(3);
    cached.now += 60;
    cached.in.tracking = true;
    cached.wait(4);
    CHECK(cached.starts == 2 && cached.logged("put on (tracking was off 63.") == 1);
    // The last status before sleeping still said "tracking" (read just before the headset came off and the Frame
    // slept), and tracking again after waking: the suspend counts as off, so it arms
    Run stale2;
    stale2.wait(5);
    stale2.finishFit();
    stale2.now += 600;
    stale2.wait(4);
    CHECK(stale2.starts == 2 && stale2.logged("put on (tracking was off 600.") == 1);
    // A gap under 10 s is not sleep: nothing
    Run shortGap;
    shortGap.wait(5);
    shortGap.finishFit();
    shortGap.now += 8;
    shortGap.wait(10);
    CHECK(shortGap.starts == 1 && shortGap.logged("put on") == 0);
}

void testDisabledDisarms() {
    // Switched off: disarmed without firing, and put-ons don't arm it
    Run run;
    run.in.enabled = false;
    run.wait(10);
    CHECK(run.starts == 0 && !run.watcher.armed());
    CHECK(run.logged("skipped (switched off)") == 1);
    run.in.tracking = false;
    run.wait(20);
    run.in.tracking = true;
    run.wait(10);
    CHECK(run.starts == 0 && !run.watcher.armed() && run.logged("put on") == 0);
    // Switched on again: from the next put-on
    run.in.enabled = true;
    run.wait(10);
    CHECK(run.starts == 0);
    run.in.tracking = false;
    run.wait(20);
    run.in.tracking = true;
    run.wait(4);
    CHECK(run.starts == 1);
    // No fit yet, or the fit values locked: nothing to do
    Run unfitted;
    unfitted.in.fitted = false;
    unfitted.wait(10);
    CHECK(unfitted.starts == 0 && unfitted.logged("no gaze fit") == 1);
    Run locked;
    locked.in.locked = true;
    locked.wait(10);
    CHECK(locked.starts == 0 && locked.logged("locked") == 1);
    // A fit started by hand while armed covers this wearing
    Run byHand;
    byHand.wait(1);
    byHand.in.fitActive = true;
    byHand.wait(20);
    byHand.in.fitActive = false;
    byHand.wait(10);
    CHECK(byHand.starts == 0 && byHand.logged("an eye fit was started") == 1);
}

void testOneShotPerPutOn() {
    // The re-center failed or was stopped: no retry until the headset is put on again
    Run run;
    run.wait(4);
    CHECK(run.starts == 1);
    run.finishFit();
    run.wait(120);
    CHECK(run.starts == 1);
    run.in.tracking = false;
    run.wait(19);
    run.in.tracking = true;
    run.wait(4);
    CHECK(run.starts == 2);
    run.finishFit();
    run.wait(120);
    CHECK(run.starts == 2);
    // One line per event, not one per update
    CHECK(run.logged("auto re-center: starting") == 2);
    CHECK(run.logged("put on") == 1);
    CHECK(run.logs.size() == 3);
}

void testCameraWarmupHolds() {
    // Valve's eye server reads the right eye shut (stuck at 0) while the eye cameras warm up: no sample would count,
    // so it waits for the warm-up, saying so once
    Run run;
    run.in.valveShut[1] = true;
    run.in.camerasWarming = true;
    run.wait(3.5);
    CHECK(run.starts == 0 && run.watcher.armed());
    CHECK(run.logged("auto re-center: waiting for the eye cameras' warm-up (Valve reads R shut)") == 1);
    run.wait(20);
    CHECK(run.starts == 0 && run.logs.size() == 1);
    // Warmed up: starts kCameraMarginSec later (frameeyeosc takes the camera's eyelid by then)
    run.in.camerasWarming = false;
    const double warm = run.now;
    run.wait(auto_recenter::kCameraMarginSec - 0.25);
    CHECK(run.starts == 0);
    run.wait(0.5);
    CHECK(run.starts == 1 && run.startedAt >= warm + auto_recenter::kCameraMarginSec);
    CHECK(run.logged("auto re-center: starting (after 2") == 1);
    // Put on again, the same: one wait (and line) per wearing
    run.finishFit();
    run.in.camerasWarming = true;
    run.in.tracking = false;
    run.wait(20);
    run.in.tracking = true;
    run.wait(10);
    CHECK(run.starts == 1 && run.logged("auto re-center: waiting") == 2);
    // ...also when the eye reads open on Valve's eye server again
    run.in.valveShut[1] = false;
    const double open = run.now;
    run.wait(2);
    CHECK(run.starts == 2 && run.startedAt >= open + auto_recenter::kCameraMarginSec);
    // The dashboard opened meanwhile: the margin ran all the same, and it starts once it has been closed a second
    Run dash;
    dash.in.valveShut[0] = true;
    dash.in.camerasWarming = true;
    dash.wait(5);
    CHECK(dash.logged("(Valve reads L shut)") == 1);
    dash.in.dashboardOpen = true;
    dash.wait(1);
    dash.in.camerasWarming = false;
    dash.wait(5);
    dash.in.dashboardOpen = false;
    const double closed = dash.now;
    dash.wait(2);
    CHECK(dash.starts == 1 && dash.startedAt >= closed + auto_recenter::kClosedSec &&
          dash.startedAt <= closed + auto_recenter::kClosedSec + 0.25);
}

void testCameraWarmupCap() {
    // Still warming up kCameraWaitSec after it would have started: it starts anyway
    Run run;
    run.in.valveShut[0] = true;
    run.in.valveShut[1] = true;
    run.in.camerasWarming = true;
    run.wait(4);
    CHECK(run.starts == 0 && run.logged("(Valve reads both eyes shut)") == 1);
    run.wait(auto_recenter::kCameraWaitSec - 1.5);
    CHECK(run.starts == 0);
    run.wait(1);
    CHECK(run.starts == 1);
    CHECK(run.startedAt >= 103.0 + auto_recenter::kCameraWaitSec &&
          run.startedAt <= 103.25 + auto_recenter::kCameraWaitSec);
    CHECK(run.logged("auto re-center: starting (the eye cameras still warming up after 45.") == 1);
    run.finishFit();
    run.wait(60);
    CHECK(run.starts == 1);
}

void testCameraWarmupOtherwiseAsBefore() {
    // An eye shut on Valve's eye server without the cameras warming up (none, ready, or no use waiting): as before
    Run noCamera;
    noCamera.in.valveShut[1] = true;
    noCamera.wait(3.5);
    CHECK(noCamera.starts == 1 && noCamera.startedAt <= 103.25 && noCamera.logged("waiting") == 0);
    // Both eyes open on it while the cameras warm up: as before
    Run open;
    open.in.camerasWarming = true;
    open.wait(3.5);
    CHECK(open.starts == 1 && open.startedAt <= 103.25 && open.logged("waiting") == 0);
    CHECK(open.logs.size() == 1 && open.logs[0] == "auto re-center: starting");
    // ...and a blink just as it would start isn't an eye shut
    Run blink;
    blink.in.camerasWarming = true;
    blink.wait(2.75);
    blink.in.valveShut[0] = true;
    blink.in.valveShut[1] = true;
    blink.wait(0.5);
    blink.in.valveShut[0] = false;
    blink.in.valveShut[1] = false;
    CHECK(blink.starts == 1 && blink.startedAt <= 103.25 && blink.logged("waiting") == 0);
}

void testWornFor() {
    // For the records: how long the eyes have been tracked, and how long they weren't before that
    Run run;
    CHECK(std::isnan(run.watcher.trackedSec()) && std::isnan(run.watcher.offBeforeSec()));  // nothing seen yet
    run.wait(6);
    CHECK(std::fabs(run.watcher.trackedSec() - 5.75) < 1e-6);
    run.in.tracking = false;
    run.wait(30);
    CHECK(std::isnan(run.watcher.trackedSec()) && std::isnan(run.watcher.offBeforeSec()));
    run.in.tracking = true;
    run.wait(10);
    CHECK(std::fabs(run.watcher.trackedSec() - 9.75) < 1e-6);
    CHECK(std::fabs(run.watcher.offBeforeSec() - 30.0) < 1e-6);
}

}  // namespace

/**
 * Run the tests.
 * @return 0 if all passed
 */
int main() {
    testStart();
    testShortGapIgnored();
    testLongGapArms();
    testSettleRestarts();
    testDashboardOpenBlocks();
    testNotConnectedWaits();
    testDashboardRetries();
    testOtherEndsNoRetry();
    testPanelFitUnaffected();
    testNotRunningFreezes();
    testSuspendCountsAsOff();
    testDisabledDisarms();
    testOneShotPerPutOn();
    testCameraWarmupHolds();
    testCameraWarmupCap();
    testCameraWarmupOtherwiseAsBefore();
    testWornFor();
    if (gFailures == 0) std::printf("auto-recenter-test: all passed\n");
    return gFailures == 0 ? 0 : 1;
}
