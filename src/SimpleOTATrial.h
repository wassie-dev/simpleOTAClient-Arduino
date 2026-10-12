/**
 * SimpleOTATrial.h - what a boot means for a saved trial record.
 *
 * The trial record uses the NVS keys that every release of this library
 * with rollback reads and writes: sota_trial (1 = trial, 2 = a rollback
 * happened) and sota_prev_part (the partition the update replaced). apply()
 * also writes sota_staged before the boot slot switches and removes it
 * straight after, so a record whose slot switch never happened can be told
 * apart from a rollback. A release that does not know sota_staged ignores
 * it, and a record written without it reads like any other, so updates in
 * either direction between releases read the same way.
 *
 * Any trial value other than 0 and 2 is a trial.
 *
 * No Arduino or ESP-IDF types, so the host tests can include this header.
 */

#ifndef SIMPLEOTA_TRIAL_H
#define SIMPLEOTA_TRIAL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SOTA_BOOT_NONE = 0,       ///< No trial record.
    SOTA_BOOT_TRIAL,          ///< Running the new image: supervise it.
    SOTA_BOOT_ROLLED_BACK,    ///< Back on the previous image: restore its identity, report.
    SOTA_BOOT_REPORT_PENDING, ///< A previous boot recorded a rollback: report it.
    SOTA_BOOT_WAITING,        ///< Flashed, not booted yet (a deep-sleep wake resumed this image).
    SOTA_BOOT_ABANDONED,      ///< The slot switch never happened: drop the record quietly.
    SOTA_BOOT_INCOMPLETE,     ///< The record cannot be checked: drop it.
    SOTA_BOOT_SLEPT_IN_TRIAL, ///< The new image woke from deep sleep before it was confirmed: roll back.
} SotaBootOutcome;

/**
 * sota_staged values. apply() writes STARTED first, then the previous
 * image's identity, then SNAPSHOT, and only then overwrites the live
 * identity with the new build's.
 */
enum {
    SOTA_STAGED_STARTED  = 1,
    SOTA_STAGED_SNAPSHOT = 2,
};

typedef struct {
    uint8_t  trial;          ///< sota_trial.
    uint8_t  staged;         ///< sota_staged, 0 when missing.
    uint32_t prevPart;       ///< sota_prev_part, 0 when missing.
    uint32_t runningPart;    ///< Address of the running partition, 0 when unknown.
    uint32_t bootPart;       ///< Address of the partition the next boot selects, 0 when unknown.
    bool     deepSleepWake;  ///< This boot is a wake from deep sleep.
} SotaBootFacts;

static inline SotaBootOutcome sotaClassifyBoot(const SotaBootFacts* f)
{
    // apply() writes sota_staged first and sota_trial last, so staged with
    // no trial is a record cut short before the slot switch.
    if (f->trial == 0) return f->staged ? SOTA_BOOT_ABANDONED : SOTA_BOOT_NONE;
    if (f->trial == 2) return SOTA_BOOT_REPORT_PENDING;
    if (f->runningPart == 0 || f->prevPart == 0) return SOTA_BOOT_INCOMPLETE;
    // A trial ends at deep sleep, as it does where the bootloader validates
    // the image on every wake: an image that sleeps before it is confirmed
    // would otherwise get a fresh confirm time on every wake, and never
    // roll back.
    if (f->runningPart != f->prevPart) {
        return f->deepSleepWake ? SOTA_BOOT_SLEPT_IN_TRIAL : SOTA_BOOT_TRIAL;
    }
    // A deep-sleep wake can resume the old image while the boot slot already
    // points at the new one, so the slot switch did happen.
    if (f->deepSleepWake && f->bootPart != 0 && f->bootPart != f->runningPart) {
        return SOTA_BOOT_WAITING;
    }
    if (f->staged) return SOTA_BOOT_ABANDONED;
    // Any other reset with a switched slot means the new image did not run,
    // which is a rollback.
    return SOTA_BOOT_ROLLED_BACK;
}

/**
 * An abandoned record puts the previous identity back only when it was
 * saved in full. Before that, the live identity was not touched.
 */
static inline bool sotaAbandonRestoresIdentity(uint8_t staged)
{
    return staged >= SOTA_STAGED_SNAPSHOT;
}

/**
 * verifyRollbackLater(): keep an image the bootloader still has on probation
 * (pending verify) only when it is a SimpleOTA trial. Any other pending
 * image, such as one written by ArduinoOTA, is left for the core to accept.
 */
static inline bool sotaHoldPendingImage(bool imagePending, SotaBootOutcome outcome)
{
    return imagePending &&
           (outcome == SOTA_BOOT_TRIAL || outcome == SOTA_BOOT_SLEPT_IN_TRIAL);
}

/** esp_reset_reason_t values used for rollback reasons. */
enum {
    SOTA_RST_POWERON   = 1,
    SOTA_RST_SW        = 3,
    SOTA_RST_PANIC     = 4,
    SOTA_RST_INT_WDT   = 5,
    SOTA_RST_TASK_WDT  = 6,
    SOTA_RST_WDT       = 7,
    SOTA_RST_DEEPSLEEP = 8,
    SOTA_RST_BROWNOUT  = 9,
};

/** Reason sent with rolled_back, from the reset that ended the trial. */
static inline const char* sotaRollbackReason(int resetReason)
{
    switch (resetReason) {
    case SOTA_RST_PANIC:     return "panic";
    case SOTA_RST_INT_WDT:   return "int_wdt";
    case SOTA_RST_TASK_WDT:  return "task_wdt";
    case SOTA_RST_WDT:       return "watchdog";
    case SOTA_RST_BROWNOUT:  return "brownout";
    case SOTA_RST_POWERON:   return "power_on";
    case SOTA_RST_DEEPSLEEP: return "deep_sleep";
    default:                 return "reset";
    }
}

#endif  // SIMPLEOTA_TRIAL_H
