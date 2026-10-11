/**
 * SimpleOTARollback.h - host-testable trial decisions.
 *
 * No Arduino or ESP-IDF types. SimpleOTAClient.cpp applies the result:
 * it marks the running image valid, starts or stops the chip watchdog,
 * and keeps the saved trial record. The native test includes this header.
 *
 * Keep verifyRollbackLater() in SimpleOTAClient.cpp. A separate file would
 * be left out of a PlatformIO archive unless something else pulled it in,
 * and the strong symbol would not replace the core's weak one.
 */

#ifndef SIMPLEOTA_ROLLBACK_H
#define SIMPLEOTA_ROLLBACK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * True only for a SimpleOTA trial this boot should keep unconfirmed:
 * the running image is awaiting acceptance, the saved note says trial,
 * a previous partition was recorded, and we are not already running it.
 * verifyRollbackLater() returns this. Every other boot returns false so
 * the Arduino core accepts the image, including one written by ArduinoOTA
 * or HTTPUpdate.
 */
static inline bool sotaHoldPendingImage(
    bool imagePending,
    bool nvsOpen,
    uint8_t trial,
    uint32_t prevPart,
    bool runningMatchesPrev)
{
    return imagePending && nvsOpen && trial == 1
        && prevPart != 0 && !runningMatchesPrev;
}

/** True when a pending image should be marked valid on this boot. */
static inline bool sotaShouldMarkImageValid(
    bool imagePending,
    bool nvsOpen,
    uint8_t trial,
    bool rollbackEnabled,
    uint32_t prevPart,
    bool runningMatchesPrev)
{
    if (!imagePending) return false;
    if (sotaHoldPendingImage(imagePending, nvsOpen, trial, prevPart,
                             runningMatchesPrev)
        && rollbackEnabled) {
        return false;
    }
    return true;
}

/**
 * What processBootValidation() does with a saved trial record.
 *
 * SOTA_TRIAL_NONE            No record, or nothing this boot must change.
 * SOTA_TRIAL_HOLD            Arm the confirm timer. Roll back only if the
 *                            image is still unconfirmed when it fires.
 * SOTA_TRIAL_CONFIRMED       The bootloader reports the image valid and this
 *                            is not the previous partition. The record is
 *                            left over from a confirm that lost power before
 *                            it was cleared. Queue the confirmed report and do
 *                            not arm the timer or the watchdog.
 * SOTA_TRIAL_ROLLED_BACK     Already back on the previous partition, or a
 *                            rollback was recorded earlier (trial == 2).
 * SOTA_TRIAL_CLEAR           The snapshot is incomplete. Drop the record.
 * SOTA_TRIAL_ACCEPT_RESIDUAL Rollback was turned off. Accept the image and
 *                            drop the leftover trial record.
 */
typedef enum SotaTrialBoot {
    SOTA_TRIAL_NONE = 0,
    SOTA_TRIAL_HOLD,
    SOTA_TRIAL_CONFIRMED,
    SOTA_TRIAL_ROLLED_BACK,
    SOTA_TRIAL_CLEAR,
    SOTA_TRIAL_ACCEPT_RESIDUAL,
} SotaTrialBoot;

static inline SotaTrialBoot sotaTrialBootAction(
    bool bootloaderRollbackCompiled,
    bool imageValid,
    bool nvsOpen,
    uint8_t trial,
    bool rollbackEnabled,
    uint32_t prevPart,
    bool runningMatchesPrev,
    bool haveRunningPartition)
{
    if (!nvsOpen || trial == 0) return SOTA_TRIAL_NONE;
    if (trial == 2) return SOTA_TRIAL_ROLLED_BACK;
    if (!rollbackEnabled) return SOTA_TRIAL_ACCEPT_RESIDUAL;
    if (trial != 1) return SOTA_TRIAL_NONE;
    if (!haveRunningPartition || prevPart == 0) return SOTA_TRIAL_CLEAR;
    if (runningMatchesPrev) return SOTA_TRIAL_ROLLED_BACK;
    /* Only an image the bootloader reports as valid was confirmed. An image
       that never became pending (bootloader built without app rollback)
       still needs the confirm timer. */
    if (bootloaderRollbackCompiled && imageValid) return SOTA_TRIAL_CONFIRMED;
    return SOTA_TRIAL_HOLD;
}

/**
 * One trial phase for the whole program. confirm, rollback, and
 * setConfirmTimeout() all read it. Only TRIAL can move, and only to
 * CONFIRMING or ROLLING_BACK, so the two writes cannot both proceed.
 */
typedef enum SotaTrialPhase {
    SOTA_PHASE_IDLE = 0,
    SOTA_PHASE_TRIAL = 1,
    SOTA_PHASE_CONFIRMING = 2,
    SOTA_PHASE_ROLLING_BACK = 3,
} SotaTrialPhase;

typedef enum SotaPhaseClaim {
    SOTA_CLAIM_CONFIRM = 0,
    SOTA_CLAIM_ROLLBACK = 1,
} SotaPhaseClaim;

static inline bool sotaClaimPhase(uint8_t phase, int claim, uint8_t* next)
{
    if (!next || phase != SOTA_PHASE_TRIAL) return false;
    if (claim == SOTA_CLAIM_CONFIRM) {
        *next = SOTA_PHASE_CONFIRMING;
        return true;
    }
    if (claim == SOTA_CLAIM_ROLLBACK) {
        *next = SOTA_PHASE_ROLLING_BACK;
        return true;
    }
    return false;
}

/** setConfirmTimeout() may restart the timer and the watchdog only in TRIAL. */
static inline bool sotaTimeoutRearmAllowed(uint8_t phase)
{
    return phase == SOTA_PHASE_TRIAL;
}

/** Accepted confirm timeouts, in seconds. */
#define SOTA_CONFIRM_TIMEOUT_MIN_S 10u
#define SOTA_CONFIRM_TIMEOUT_MAX_S 86400u

static inline uint32_t sotaClampConfirmTimeout(uint32_t seconds)
{
    if (seconds < SOTA_CONFIRM_TIMEOUT_MIN_S) return SOTA_CONFIRM_TIMEOUT_MIN_S;
    if (seconds > SOTA_CONFIRM_TIMEOUT_MAX_S) return SOTA_CONFIRM_TIMEOUT_MAX_S;
    return seconds;
}

/**
 * Confirm-timer period in FreeRTOS ticks. The multiply is 64-bit because
 * pdMS_TO_TICKS() multiplies in 32 bits and wraps past 4294 seconds when
 * the tick rate is 1000 Hz.
 */
static inline uint32_t sotaConfirmTimerTicks(uint32_t timeoutSec, uint32_t tickHz)
{
    uint64_t ticks = (uint64_t)timeoutSec * (uint64_t)tickHz;
    if (ticks > 0xFFFFFFFFu) return 0xFFFFFFFFu;
    return (uint32_t)ticks;
}

/** Extra watchdog time: the larger of floorSec and 10 percent of the deadline. */
static inline uint32_t sotaWatchMarginSec(uint32_t confirmSec, uint32_t floorSec)
{
    uint32_t tenth = confirmSec / 10u;
    return tenth > floorSec ? tenth : floorSec;
}

/**
 * Slow-clock ticks for a duration. calQ13_19 is the cached calibration
 * (microseconds per tick, Q13.19). Returns 0 when cal is 0 so the caller
 * can fall back to the nominal frequency, and saturates instead of
 * overflowing.
 */
static inline uint64_t sotaSlowclkTicks(uint64_t timeoutUs, uint32_t calQ13_19)
{
    if (calQ13_19 == 0) return 0;
    if (timeoutUs > (UINT64_MAX >> 19)) return UINT64_MAX;
    return (timeoutUs << 19) / (uint64_t)calQ13_19;
}

/**
 * Nominal slow-clock ticks, used when the calibration value is still 0.
 * The result is clamped to the 32-bit stage and is at least 2 ticks.
 */
static inline uint32_t sotaNominalSlowTicks(uint64_t seconds, uint32_t hz)
{
    if (hz == 0) hz = 1;
    uint64_t ticks = seconds * (uint64_t)hz;
    if (ticks > 0xFFFFFFFFu) return 0xFFFFFFFFu;
    if (ticks < 2) return 2;
    return (uint32_t)ticks;
}

/* Numeric values match esp_reset_reason_t in esp_system.h. */
enum {
    SOTA_RST_UNKNOWN    = 0,
    SOTA_RST_POWERON    = 1,
    SOTA_RST_EXT        = 2,
    SOTA_RST_SW         = 3,
    SOTA_RST_PANIC      = 4,
    SOTA_RST_INT_WDT    = 5,
    SOTA_RST_TASK_WDT   = 6,
    SOTA_RST_WDT        = 7,
    SOTA_RST_DEEPSLEEP  = 8,
    SOTA_RST_BROWNOUT   = 9,
};

/**
 * Reason to save on the boot that detects the rollback. A reason already
 * stored before this library reboots wins, so a software reset from the
 * confirm timer stays confirm_timeout. Otherwise this boot's reset reason
 * is used. Call this while detecting the rollback, then persist the result.
 */
static inline const char* sotaRollbackReason(int resetCode, const char* stored)
{
    if (stored && stored[0] != '\0') return stored;
    switch (resetCode) {
        case SOTA_RST_PANIC:     return "panic";
        case SOTA_RST_INT_WDT:   return "int_wdt";
        case SOTA_RST_TASK_WDT:  return "task_wdt";
        case SOTA_RST_WDT:       return "watchdog";
        case SOTA_RST_BROWNOUT:  return "brownout";
        case SOTA_RST_POWERON:   return "power_on";
        case SOTA_RST_DEEPSLEEP: return "deep_sleep";
        case SOTA_RST_SW:        return "reset";
        default:                 return "reset";
    }
}

/**
 * Reason to send with a rolled_back report. This is only the value saved
 * when the rollback was detected. It does not look at the reset reason of
 * the boot that finally reaches the server.
 */
static inline const char* sotaReportRollbackReason(const char* stored)
{
    if (stored && stored[0] != '\0') return stored;
    return "reset";
}

/**
 * apply() before any download.
 *
 * A flashed image waiting for rebootForUpdate() is first: a second apply()
 * with no new offer still returns that pending result.
 * An unaccepted rolled_back report is next, including when no offer is
 * stored. apply() then keeps the offer, flashes nothing, and reports
 * nothing, so the trial record and its reason stay as they are.
 */
typedef enum SotaApplyGuard {
    SOTA_APPLY_REBOOT_PENDING = 1,
    SOTA_APPLY_ROLLBACK_PENDING = 2,
    SOTA_APPLY_NO_OFFER = 3,
    SOTA_APPLY_PROCEED = 4,
} SotaApplyGuard;

static inline SotaApplyGuard sotaApplyGuard(
    bool rebootPending,
    bool rollbackReportPending,
    bool hasOffer)
{
    if (rebootPending) return SOTA_APPLY_REBOOT_PENDING;
    if (rollbackReportPending) return SOTA_APPLY_ROLLBACK_PENDING;
    if (!hasOffer) return SOTA_APPLY_NO_OFFER;
    return SOTA_APPLY_PROCEED;
}

/**
 * Deployment used for the reboot event. While a reboot is pending this is
 * the flashed deployment, which is a different slot from the current offer.
 * The pointers are the ones passed in.
 */
typedef struct SotaStatusTarget {
    const char* deploymentId;
    uint32_t buildNumber;
} SotaStatusTarget;

static inline SotaStatusTarget sotaStatusTarget(
    bool rebootPending,
    const char* rebootDeploymentId,
    uint32_t rebootBuild,
    const char* offerDeploymentId,
    uint32_t offerBuild)
{
    SotaStatusTarget target;
    if (rebootPending && rebootDeploymentId && rebootDeploymentId[0] != '\0') {
        target.deploymentId = rebootDeploymentId;
        target.buildNumber = rebootBuild;
        return target;
    }
    target.deploymentId = offerDeploymentId ? offerDeploymentId : "";
    target.buildNumber = offerBuild;
    return target;
}

/**
 * How a status POST ended, for reports that are retried until the server
 * answers.
 *
 * SOTA_STATUS_ACCEPTED  2xx with accepted=true.
 * SOTA_STATUS_RETRY     No response, a 5xx, 401, 403, 408, or 429, or a
 *                       2xx without an accepted field. Worth sending again.
 * SOTA_STATUS_REJECTED  2xx with accepted=false, or any other 4xx. Sending
 *                       the same report again gets the same answer.
 */
typedef enum SotaStatusOutcome {
    SOTA_STATUS_ACCEPTED = 1,
    SOTA_STATUS_RETRY = 2,
    SOTA_STATUS_REJECTED = 3,
} SotaStatusOutcome;

static inline SotaStatusOutcome sotaStatusOutcome(
    int httpCode, bool hasAcceptedField, bool accepted)
{
    if (httpCode >= 200 && httpCode < 300) {
        if (!hasAcceptedField) return SOTA_STATUS_RETRY;
        return accepted ? SOTA_STATUS_ACCEPTED : SOTA_STATUS_REJECTED;
    }
    if (httpCode >= 400 && httpCode < 500) {
        if (httpCode == 401 || httpCode == 403 || httpCode == 408 || httpCode == 429) {
            return SOTA_STATUS_RETRY;
        }
        return SOTA_STATUS_REJECTED;
    }
    return SOTA_STATUS_RETRY;
}

/**
 * The rolled_back trial record and sota_rb_why are removed only when the
 * server accepted the report. A retry or a rejection leaves both in place.
 * apply() does not remove the reason when it arms a later trial.
 */
static inline bool sotaRollbackReportAccepted(int outcome)
{
    return outcome == SOTA_STATUS_ACCEPTED;
}

static inline bool sotaClearRollbackReason(bool reportAccepted)
{
    return reportAccepted;
}

static inline bool sotaClearTrialOnRollbackAccept(bool reportAccepted)
{
    return reportAccepted;
}

/**
 * True when the saved rolled_back has a deployment and a build that can
 * be sent. An empty record cannot be reported.
 */
static inline bool sotaRollbackRecordReportable(
    const char* deploymentId,
    uint32_t buildNumber)
{
    return deploymentId && deploymentId[0] != '\0' && buildNumber != 0;
}

/*
 * The chip watchdog is armed from SimpleOTAWdt.c, which is C. On Arduino
 * core 2.0.17 the S2 and C3 mwdt_ll.h header does not compile as C++.
 * verifyRollbackLater() stays in SimpleOTAClient.cpp.
 */
#ifdef __cplusplus
extern "C" {
#endif
void sota_wdt_arm(uint32_t confirm_sec);
void sota_wdt_disarm(void);
void sota_wdt_quiesce(void);
bool sota_wdt_is_armed(void);
bool sota_wdt_is_clamped(void);
#ifdef __cplusplus
}
#endif

#endif  // SIMPLEOTA_ROLLBACK_H
