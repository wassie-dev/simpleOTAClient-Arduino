/**
 * Host-side unit tests for SimpleOTATrial.h and SimpleOTAOutbox.h.
 *
 *   g++ -std=c++11 -Wall -Wextra -Werror -Isrc -o test_trial test/native/test_trial.cpp && ./test_trial
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "SimpleOTAOutbox.h"
#include "SimpleOTATrial.h"

static int g_failures = 0;

#define CHECK(cond, name)                                        \
    do {                                                         \
        if (cond) {                                              \
            printf("PASS  %s\n", name);                          \
        } else {                                                 \
            printf("FAIL  %s (line %d)\n", name, __LINE__);      \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

static const uint32_t A = 0x10000;  // previous image
static const uint32_t B = 0x150000; // new image

static SotaBootOutcome classify(uint8_t trial, uint8_t staged, uint32_t prev,
                                uint32_t running, uint32_t boot, bool deepSleep) {
    SotaBootFacts f = {};
    f.trial = trial;
    f.staged = staged;
    f.prevPart = prev;
    f.runningPart = running;
    f.bootPart = boot;
    f.deepSleepWake = deepSleep;
    return sotaClassifyBoot(&f);
}

int main(void) {
    // Records written without sota_staged.
    CHECK(classify(0, false, 0, A, A, false) == SOTA_BOOT_NONE, "no record");
    CHECK(classify(1, false, A, B, B, false) == SOTA_BOOT_TRIAL,
          "legacy record, running the new image: trial");
    CHECK(classify(1, false, A, A, A, false) == SOTA_BOOT_ROLLED_BACK,
          "legacy record, back on the previous image: rolled back");
    CHECK(classify(2, false, A, A, A, false) == SOTA_BOOT_REPORT_PENDING,
          "trial 2 from an earlier boot: report pending");
    CHECK(classify(7, false, A, B, B, false) == SOTA_BOOT_TRIAL,
          "any trial value other than 0 and 2 is a trial");
    CHECK(classify(1, false, 0, B, B, false) == SOTA_BOOT_INCOMPLETE,
          "no previous partition: cannot be checked");
    CHECK(classify(1, false, A, 0, 0, false) == SOTA_BOOT_INCOMPLETE,
          "running partition unknown: cannot be checked");

    // Power cut at each point of apply(): staged first, trial last, then the
    // slot switch, then sota_staged removed.
    CHECK(classify(0, true, A, A, A, false) == SOTA_BOOT_ABANDONED,
          "cut after staged, before trial: abandoned, no false confirmed report");
    CHECK(classify(1, true, A, A, A, false) == SOTA_BOOT_ABANDONED,
          "cut after trial, before the slot switch: abandoned, not a rollback");
    CHECK(classify(1, true, A, B, B, false) == SOTA_BOOT_TRIAL,
          "cut after the slot switch, before unstaging: trial");

    // Deferred reboot and deep sleep.
    CHECK(classify(1, false, A, A, B, true) == SOTA_BOOT_WAITING,
          "deep-sleep wake on the old image, slot already switched: waiting");
    CHECK(classify(1, false, A, A, B, false) == SOTA_BOOT_ROLLED_BACK,
          "any other reset with a switched slot: the new image did not run");
    CHECK(classify(1, false, A, A, A, true) == SOTA_BOOT_ROLLED_BACK,
          "deep-sleep wake with the slot back on the old image: rolled back");
    CHECK(classify(1, true, A, A, B, true) == SOTA_BOOT_WAITING,
          "slot switched, sota_staged left behind, deep-sleep wake: waiting");
    CHECK(classify(1, false, A, A, 0, true) == SOTA_BOOT_ROLLED_BACK,
          "deep-sleep wake, boot partition unknown: rolled back");

    // The snapshot of the previous identity: a power cut at any write before
    // the slot switch must leave the live identity as it was.
    enum { W_STARTED, W_PREV_PART, W_PREV_BUILD, W_PREV_HASH, W_PREV_VER,
           W_SNAPSHOT, W_BUILD, W_HASH, W_VER, W_FAIL_DEP, W_FAIL_BUILD,
           W_CONF_PEND, W_TRIAL, W_COUNT };
    bool keepsIdentity = true;
    for (int cut = 0; cut <= W_COUNT; ++cut) {
        uint8_t staged = 0, trial = 0;
        uint32_t prevPart = 0, prevBuild = 0, build = 41;
        for (int w = 0; w < cut; ++w) {
            if (w == W_STARTED)    staged = SOTA_STAGED_STARTED;
            if (w == W_PREV_PART)  prevPart = A;
            if (w == W_PREV_BUILD) prevBuild = 41;
            if (w == W_SNAPSHOT)   staged = SOTA_STAGED_SNAPSHOT;
            if (w == W_BUILD)      build = 42;
            if (w == W_TRIAL)      trial = 1;
        }
        const SotaBootOutcome o = classify(trial, staged, prevPart, A, A, false);
        if (o == SOTA_BOOT_ABANDONED && sotaAbandonRestoresIdentity(staged)) build = prevBuild;
        keepsIdentity = keepsIdentity &&
                        (cut == 0 ? o == SOTA_BOOT_NONE
                                  : o == SOTA_BOOT_ABANDONED || o == SOTA_BOOT_INCOMPLETE) &&
                        build == 41;
    }
    CHECK(keepsIdentity, "a power cut at any write before the slot switch keeps the identity");
    CHECK(!sotaAbandonRestoresIdentity(0) && !sotaAbandonRestoresIdentity(SOTA_STAGED_STARTED) &&
              sotaAbandonRestoresIdentity(SOTA_STAGED_SNAPSHOT),
          "only a full snapshot is restored");

    // Outbox.
    SotaOutbox box;
    sotaOutboxInit(&box);
    CHECK(sotaOutboxValid(&box) && box.count == 0, "a new outbox is empty");
    CHECK(sotaOutboxPush(&box, "rolled_back", "confirm_timeout", "dep-a", 7) &&
              box.count == 1,
          "a report is queued");
    CHECK(!sotaOutboxPush(&box, "confirmed", nullptr, "", 8) && box.count == 1,
          "a report without a deployment is not queued");
    CHECK(!sotaOutboxPush(&box, "confirmed", nullptr, "dep-b", 0) && box.count == 1,
          "a report without a build is not queued");
    char longDep[80];
    memset(longDep, 'x', sizeof(longDep) - 1);
    longDep[sizeof(longDep) - 1] = '\0';
    CHECK(!sotaOutboxPush(&box, "confirmed", nullptr, longDep, 8) && box.count == 1,
          "a deployment id that does not fit is not queued");
    CHECK(sotaOutboxPush(&box, "confirmed", nullptr, "dep-b", 8) && box.count == 2,
          "a second report is queued");
    CHECK(sotaOutboxPush(&box, "confirmed", nullptr, "dep-c", 9) && box.count == 2 &&
              strcmp(box.reports[0].deployment, "dep-b") == 0 &&
              strcmp(box.reports[1].deployment, "dep-c") == 0,
          "a full outbox drops its oldest report");
    sotaOutboxPop(&box);
    CHECK(box.count == 1 && strcmp(box.reports[0].deployment, "dep-c") == 0 &&
              box.reports[0].build == 9 && box.reports[0].reason[0] == '\0',
          "pop removes the oldest report");
    sotaOutboxPop(&box);
    sotaOutboxPop(&box);
    CHECK(box.count == 0, "pop on an empty outbox is harmless");
    box.version = 99;
    CHECK(!sotaOutboxValid(&box), "an outbox with another layout is not loaded");

    sotaOutboxInit(&box);
    CHECK(!sotaOutboxPush(&box, nullptr, nullptr, "d", 1) && !sotaOutboxPush(&box, "", nullptr, "d", 1) &&
              box.count == 0,
          "a report without an event is not queued");
    CHECK(sotaOutboxPush(&box, "confirmed", nullptr, "d", UINT32_MAX) &&
              strcmp(box.reports[0].event, "confirmed") == 0 && box.reports[0].build == UINT32_MAX,
          "a queued report keeps its fields");
    sotaOutboxPush(&box, "confirmed", nullptr, "d", 2);
    CHECK(box.count == SOTA_OUTBOX_SLOTS && sotaOutboxValid(&box), "a full outbox is valid");
    box.count = SOTA_OUTBOX_SLOTS + 1;
    CHECK(!sotaOutboxValid(&box), "a count past the slots is not loaded");
    sotaOutboxInit(&box);
    box.count = 1;
    CHECK(!sotaOutboxValid(&box), "an empty loaded report is not loaded");
    sotaOutboxPush(&box, "confirmed", nullptr, "d", 3);
    memset(box.reports[0].event, 'x', sizeof(box.reports[0].event));
    CHECK(!sotaOutboxValid(&box), "a loaded report with an unterminated field is not loaded");
    CHECK(sizeof(SotaReport) == 104 && sizeof(SotaOutbox) == 212 &&
              offsetof(SotaOutbox, reports) == 4 && SOTA_OUTBOX_VERSION == 1u,
          "outbox blob layout (change SOTA_OUTBOX_VERSION with it)");

    CHECK(sotaReportDelivered(200) && sotaReportDelivered(404) &&
              sotaReportDelivered(503),
          "any HTTP response delivers a report");
    CHECK(!sotaReportDelivered(-1) && !sotaReportDelivered(0),
          "no response keeps the report");

    printf("===========================\n");
    if (g_failures) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
