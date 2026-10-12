#ifndef KRYONOS_WATCHDOG_FEED_H
#define KRYONOS_WATCHDOG_FEED_H

#include <esp_task_wdt.h>

// Feed the task watchdog, and do nothing at all when the calling task is not subscribed to it.
//
// WHY THIS IS NOT A BARE esp_task_wdt_reset()
//   esp_task_wdt_reset() is only meaningful for a task that has been added to the TWDT. Called from a
//   task that was not, IDF refuses it and logs
//       E (4404) task_wdt: esp_task_wdt_reset(707): task not found
//   on EVERY call -- so in a poll loop the line arrives at the loop's own rate, not once. That is
//   what the Korvo-1's capture showed: 38 lines, 200 ms apart, for the whole WiFi association
//   (handoff/project.md §7.3).
//
// WHY IT ONLY STARTED SHOWING UP ON ONE BOARD
//   Whether the Arduino loop task is subscribed is a property of the core, not of the call. Under
//   Arduino 3.3.12 / IDF 5.5.5 -- every environment but the S31 -- the loop task is in the TWDT, so
//   the bare call was correct and silent. Under Arduino 4.0.0-RC1 / IDF 6.1, the ESP32-S31 core, it
//   is not, and the same call became an error stream. A guard gets both: the feed still happens where
//   the task is watched, and where it is not, the call is skipped rather than refused.
//
// WHY THE QUESTION IS ASKED PER CALL
//   A subscription is per-task. The caller may be the loop task, a server task or an app's own, and
//   only the task holding the handle can answer for itself -- esp_task_wdt_status(nullptr) means "the
//   current task". So this is a query at the call site rather than a flag cached once at startup,
//   which is the version that would be wrong the first time two tasks disagreed. ESP_ERR_INVALID_STATE
//   (the TWDT was never initialized) takes the skip path too, which is right for the same reason:
//   there is nothing to feed.
inline void kryonFeedWatchdog() {
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

#endif // KRYONOS_WATCHDOG_FEED_H
