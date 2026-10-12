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
    if (f->runningPart != f->prevPart) return SOTA_BOOT_TRIAL;
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

#endif  // SIMPLEOTA_TRIAL_H
