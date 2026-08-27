/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_RECOVERY_POLICY_H
#define VIDEOCOMPOSER_RECOVERY_POLICY_H

namespace videocomposer {

/**
 * RecoveryPolicy - when a decode queue may be reopened, and when it is over.
 *
 * The arithmetic that decides whether the recovery worker runs another episode
 * or declares the layer permanently failed, isolated from the worker so it can
 * be unit-tested without threads, a GPU, or a damaged file.
 *
 * **Why this exists.** The first version of the worker counted *attempts*, and
 * the counter was the for-loop index of a single wake: every wake started a
 * fresh ladder at attempt 1. Attempt 1 is a full-pool reopen, which always
 * succeeds for the only fault class that actually reaches recovery (stream
 * corruption - a damaged file is still demuxable). So a persistently damaged
 * file cycled unhealthy -> reopen -> ~28 good frames -> unhealthy forever at
 * ~1 Hz, rebuilding a VAAPI context and surface pool every cycle. That is the
 * allocation-burst pattern the vcn_dec ring hang (869en65tm) is measured
 * against: the machinery meant to rescue one layer was a plausible way to take
 * down the whole compositor. `declared_failed` was unreachable for that class,
 * because it was only set when all three reopens *within one wake* failed.
 *
 * **The fix is to count episodes, not attempts, and to decay on evidence.**
 * An *episode* is one wake-with-work. The run of episodes resets only when the
 * layer has actually delivered pictures - not after a period of wall-clock.
 * Wall-clock is the wrong instrument here: the worker is only ever woken from
 * the renderer's frame-miss path, which runs only while the frame advances, so
 * "time since the last episode" can span arbitrarily long intervals in which
 * the queue was unhealthy and nobody looked. A stinger cue on a damaged file,
 * fired every few minutes, would decay every time and never declare. Good
 * frames cannot be faked by waiting: a stalled queue produces none.
 *
 * Sizing (both measured against the observed ~1.1 s limp cycle - 28 good
 * frames at 25 fps plus the reopen):
 *
 *  - cap 3 bounds one fault run to at most three context rebuilds, reaching
 *    the declaration within single-digit seconds of the stimulus.
 *  - 750 good frames is ~30 s of delivered picture at 25 fps. A persistent
 *    limper yields ~28 per episode, 27x short, so it cannot game the counter;
 *    a file that plays 750 frames cleanly after a rescue has earned a fresh
 *    ladder. Frame-based rather than seconds, deliberately: the framerate
 *    cancels out of that 27x ratio, and a paused or stalled queue simply never
 *    decays. Two consequences worth knowing: the window is media-native, so it
 *    is ~15 s on 50 fps content, and a seek or jump discards decode-ahead
 *    frames, inflating the count by at most one queue depth (<= 8) per event -
 *    immaterial at this scale, and loop wraps convert in place and discard
 *    nothing.
 *
 * The instance is owned by VideoFileInput and touched **only by the recovery
 * worker between wake and park**, so no synchronisation is needed here. (The
 * queue's own quiesce flag is a different matter and is atomic - the decode
 * thread reads it with no join relationship.)
 */
struct RecoveryPolicy {
    /** Episodes allowed in one fault run before the layer is declared failed. */
    static constexpr int RECOVERY_EPISODE_CAP = 3;

    /**
     * Good frames that must be delivered after an episode for the run to be
     * considered over. Media-native frames, not seconds - see the class note.
     */
    static constexpr long long RECOVERY_DECAY_GOOD_FRAMES = 750;

    /** What the worker should do with this wake. */
    enum class Decision {
        attempt,  ///< run an episode (reopen ladder)
        declare   ///< cap reached: declare the layer permanently failed
    };

    int episodesInRun = 0;

    /**
     * Decide this wake, and account for it.
     *
     * @param goodFramesSinceLastEpisode frames the queue decoded since the
     *        reopen that ended the previous episode. The queue's counter is
     *        reset by open(), so at episode start it already means exactly
     *        this - the caller passes it straight through.
     * @return attempt (and the episode is counted) or declare (and nothing is
     *         consumed - the cap check must not spend an attempt).
     */
    Decision onWake(long long goodFramesSinceLastEpisode) {
        if (goodFramesSinceLastEpisode >= RECOVERY_DECAY_GOOD_FRAMES) {
            episodesInRun = 0;
        }
        if (episodesInRun >= RECOVERY_EPISODE_CAP) {
            return Decision::declare;
        }
        ++episodesInRun;
        return Decision::attempt;
    }

    /** Episodes consumed so far in the current run (1-based once running). */
    int episodeNumber() const { return episodesInRun; }
};

}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_RECOVERY_POLICY_H
