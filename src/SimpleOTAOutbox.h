/**
 * SimpleOTAOutbox.h - status reports waiting to be sent.
 *
 * "confirmed" and "rolled_back" are telemetry. Once a boot has worked out
 * what happened to a trial, the report moves here and the trial keys are
 * cleared, so a later update can never overwrite a report that has not
 * been sent, and a pending report never holds up an update.
 *
 * A report is delivered once the server returns any HTTP response; what
 * the response says does not matter. Without a response it stays here and
 * is sent again on the next check. When the outbox is full, the oldest
 * report makes room for the new one.
 *
 * The struct is stored as one NVS blob. No Arduino or ESP-IDF types, so
 * the host tests can include this header.
 */

#ifndef SIMPLEOTA_OUTBOX_H
#define SIMPLEOTA_OUTBOX_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SOTA_OUTBOX_VERSION 1u
#define SOTA_OUTBOX_SLOTS   2u

typedef struct {
    char     event[12];       ///< "confirmed" or "rolled_back".
    char     reason[24];      ///< Empty when the event has none.
    char     deployment[64];
    uint32_t build;
} SotaReport;

typedef struct {
    uint8_t    version;       ///< SOTA_OUTBOX_VERSION.
    uint8_t    count;
    SotaReport reports[SOTA_OUTBOX_SLOTS];
} SotaOutbox;

/** Copy src into a fixed field. Returns false when it does not fit. */
static inline bool sotaCopyField(char* dst, size_t cap, const char* src)
{
    const size_t len = src ? strlen(src) : 0;
    if (len >= cap) return false;
    if (len) memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static inline void sotaOutboxInit(SotaOutbox* box)
{
    memset(box, 0, sizeof(*box));
    box->version = SOTA_OUTBOX_VERSION;
}

static inline bool sotaFieldTerminated(const char* field, size_t cap)
{
    return memchr(field, '\0', cap) != NULL;
}

/**
 * True when a loaded blob has this layout, a count it can hold, and only
 * reports that sotaOutboxPush() could have added.
 */
static inline bool sotaOutboxValid(const SotaOutbox* box)
{
    if (box->version != SOTA_OUTBOX_VERSION || box->count > SOTA_OUTBOX_SLOTS) return false;
    for (uint8_t i = 0; i < box->count; ++i) {
        const SotaReport* r = &box->reports[i];
        if (!sotaFieldTerminated(r->event, sizeof(r->event)) ||
            !sotaFieldTerminated(r->reason, sizeof(r->reason)) ||
            !sotaFieldTerminated(r->deployment, sizeof(r->deployment)) ||
            !r->event[0] || !r->deployment[0] || r->build == 0) {
            return false;
        }
    }
    return true;
}

/**
 * Add a report at the back. A report without an event, deployment or
 * build, or whose fields do not fit, is not added. Returns false in that
 * case.
 */
static inline bool sotaOutboxPush(SotaOutbox* box, const char* event,
                                  const char* reason, const char* deployment,
                                  uint32_t build)
{
    SotaReport r;
    memset(&r, 0, sizeof(r));
    if (!event || !event[0] || !deployment || !deployment[0] || build == 0) return false;
    if (!sotaCopyField(r.event, sizeof(r.event), event) ||
        !sotaCopyField(r.reason, sizeof(r.reason), reason) ||
        !sotaCopyField(r.deployment, sizeof(r.deployment), deployment)) {
        return false;
    }
    r.build = build;
    if (box->count == SOTA_OUTBOX_SLOTS) {
        memmove(&box->reports[0], &box->reports[1],
                sizeof(SotaReport) * (SOTA_OUTBOX_SLOTS - 1));
        box->count--;
    }
    box->reports[box->count++] = r;
    return true;
}

/** Remove the report at the front. */
static inline void sotaOutboxPop(SotaOutbox* box)
{
    if (box->count == 0) return;
    memmove(&box->reports[0], &box->reports[1],
            sizeof(SotaReport) * (SOTA_OUTBOX_SLOTS - 1));
    box->count--;
    memset(&box->reports[box->count], 0, sizeof(SotaReport));
}

/** A report is delivered once the server returns any HTTP response. */
static inline bool sotaReportDelivered(int httpStatus)
{
    return httpStatus > 0;
}

#endif  // SIMPLEOTA_OUTBOX_H
