#include "auto_recenter.h"

#include <algorithm>
#include <cstdio>

namespace auto_recenter {

std::string Watcher::disarm(const char* why) {
    armed_ = false;
    return std::string("auto re-center: skipped (") + why + ")";
}

std::string Watcher::fitEnded(bool byItself, bool byDashboard) {
    if (!byItself || !byDashboard) return std::string();
    char text[160];
    if (!willRetry(byItself, byDashboard)) {
        armed_ = false;
        std::snprintf(text, sizeof(text),
                      "auto re-center: stopped by the dashboard opening; not trying again (%d retries used this wearing)",
                      retries_);
        return text;
    }
    ++retries_;
    armed_ = true;
    std::snprintf(text, sizeof(text),
                  "auto re-center: stopped by the dashboard opening; trying again once it has been closed %.0f s "
                  "(retry %d of %d)",
                  kClosedSec, retries_, kDashboardRetries);
    return text;
}

Step Watcher::update(double now, const Inputs& in) {
    Step step;
    const double dt = started_ ? std::max(0.0, now - lastNow_) : 0.0;
    started_ = true;
    lastNow_ = now;
    // Not connected to SteamVR, the dashboard reads closed but can't be known: it counts as open, so the time it has
    // been closed starts when the panel connects
    const bool dashboardOpen = in.dashboardOpen || !in.vrConnected;
    if (dashboardOpen != dashboardOpen_) {
        dashboardOpen_ = dashboardOpen;
        if (!dashboardOpen) closedAt_ = now;
    }
    if (dt > kSleepGapSec) {
        // No update for this long: the Frame slept (the loop runs every 0.25-0.5 s), which it does with the headset
        // off, and the eye server delivers nothing through a suspend. So it counts as off whatever the last status
        // said: also when that was read just before sleeping and still said "tracking", which otherwise made the
        // whole suspend count as worn and missed the wearing
        if (tracking_) {
            tracking_ = false;
            offFor_ = 0.0;
        }
        offFor_ += dt;
        onFor_ = 0.0;
    } else if (wasRunning_) {
        // The time since the last update counts for what was seen then, if frameeyeosc ran. So the time the Frame
        // slept after the headset came off counts as off, even when the first status after waking up is still the
        // stale one from before (read as "not running") or has not been read again yet
        (tracking_ ? onFor_ : offFor_) += dt;
    }
    // Changes only while frameeyeosc runs: its status says nothing about the eyes otherwise, and a restart while the
    // headset is worn must not look like putting it on (the time it was not running counts for nothing)
    if (in.running) {
        if (in.tracking != tracking_) {
            if (in.tracking) {
                // Back after a real break (not one of the tracker's hiccups): the headset was put on
                if (offFor_ >= kOffSec && in.enabled && in.fitted && !in.locked) {
                    armed_ = true;
                    retries_ = 0;
                    holdSince_ = NAN;
                    clearSince_ = NAN;
                    char text[96];
                    std::snprintf(text, sizeof(text), "put on (tracking was off %.1f s): re-centering once the eyes settle",
                                  offFor_);
                    step.log = text;
                }
                onFor_ = 0.0;
            } else {
                // Still adjusting the headset: wait for the eyes to settle again
                if (armed_) step.log = "auto re-center: eyes lost, waiting for them to settle again";
                offFor_ = 0.0;
            }
            tracking_ = in.tracking;
        }
    }
    wasRunning_ = in.running;
    // How long an eye has read shut on Valve's eye server while tracked (a blink is not long enough)
    if (!(in.running && tracking_ && (in.valveShut[0] || in.valveShut[1]))) {
        shutSince_ = NAN;
    } else if (std::isnan(shutSince_)) {
        shutSince_ = now;
    }
    if (!armed_) return step;
    // Such an eye counts in the fit only once the eye camera's eyelid takes over for it, which it doesn't while the
    // eye cameras warm up: then no sample would count. Once held back for that, whether it still holds
    const bool hold = !std::isnan(shutSince_) && now - shutSince_ >= kShutSec && in.camerasWarming;
    if (!std::isnan(holdSince_)) {
        if (hold) {
            clearSince_ = NAN;
        } else if (std::isnan(clearSince_)) {
            clearSince_ = now;
        }
    }
    // Nothing to re-center, or it can't run: this wearing is left alone
    if (!in.enabled) {
        step.log = disarm("switched off");
    } else if (!in.fitted) {
        step.log = disarm("no gaze fit yet");
    } else if (in.locked) {
        step.log = disarm("fit values locked by the command line");
    } else if (in.fitActive) {
        step.log = disarm("an eye fit was started");
    } else if (in.running && tracking_ && onFor_ >= kSettleSec && !dashboardOpen && now - closedAt_ >= kClosedSec) {
        // Held back until the warm-up is over (and kCameraMarginSec more, for frameeyeosc to take the camera's
        // eyelid), kCameraWaitSec at most
        if (hold && std::isnan(holdSince_)) {
            holdSince_ = now;
            const char* eyes = in.valveShut[0] && in.valveShut[1] ? "both eyes" : in.valveShut[0] ? "L" : "R";
            step.log = std::string("auto re-center: waiting for the eye cameras' warm-up (Valve reads ") + eyes +
                       " shut)";
        }
        const bool held = !std::isnan(holdSince_);
        if (held && now - holdSince_ < kCameraWaitSec && (hold || now - clearSince_ < kCameraMarginSec)) return step;
        // One try per wearing: no retry if it fails or is stopped, unless the dashboard opening stopped it
        // (fitEnded arms it again)
        armed_ = false;
        step.start = true;
        step.log = "auto re-center: starting";
        if (retries_ > 0) {
            char text[48];
            std::snprintf(text, sizeof(text), " (retry %d of %d)", retries_, kDashboardRetries);
            step.log += text;
        }
        if (held) {
            // (the record's logs start shortly before it: the wait is said here too)
            char text[96];
            std::snprintf(text, sizeof(text), hold ? " (the eye cameras still warming up after %.1f s)"
                                                   : " (after %.1f s waiting for the eye cameras' warm-up)",
                          now - holdSince_);
            step.log += text;
        }
    }
    return step;
}

}  // namespace auto_recenter
