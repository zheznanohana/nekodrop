// SPDX-License-Identifier: GPL-2.0-only
/* Small, testable policy for the station's host TX queue: whether frames may
 * leave the host now, and whether the radio main loop should treat the station
 * as busy. Keeping this pure makes the yield/pause and BA-teardown cases
 * testable without a card. */
#pragma once

namespace sta_tx {

inline bool can_send(bool running, bool yielded, bool paused) {
  return running && !yielded && !paused;
}

inline bool pending(bool running, bool yielded, bool paused, bool host_queue, unsigned firmware_queue) {
  return can_send(running, yielded, paused) && (host_queue || firmware_queue > 0);
}

} // namespace sta_tx
