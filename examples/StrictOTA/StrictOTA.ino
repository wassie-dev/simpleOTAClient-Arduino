// StrictOTA: managed updates with strict signing.
//
// Signed mode (see SignedOTA) verifies each image against your PUBLIC
// signing key. Strict signing also checks a second signature, made by the
// SimpleOTA CLI with the same key, over your project id, the key id, the
// build number and the image's SHA-256. It is checked before anything is
// downloaded, so a server that was taken over cannot pass an old signed
// image off as a newer build, or offer another project's image signed
// with a shared key. Such an update is refused (SIMPLEOTA_SIGNATURE_FAIL,
// reason signature_invalid) and the device keeps its current firmware.
//
// Setup (see https://wiki.simpleota.com/guides/signed-firmware/):
//   1. Paste your project's PUBLIC signing key below, and your project's
//      UUID (lowercase with hyphens, as the CLI's key snippet prints it;
//      not the project's slug). The UUID is not a secret.
//   2. Release builds with the SimpleOTA CLI, which makes them
//      strict-ready. A device in strict signing is only offered those.
//
// Moving a signed fleet here? Ship THIS firmware as one last signed build.

#include <WiFi.h>
#include <simpleota.h>

static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-password";

static const char* SOTA_TOKEN = "soto_proj_xxxx";   // project or device token

#define FIRMWARE_VERSION "1.0.0"

// The placeholders below intentionally do NOT pass simpleota_init(): if you
// flash this sketch without pasting your own values, setup() halts with a
// clear error instead of running.
static const char* PROJECT_ID = "your-project-uuid";

static const char* SIGNING_PUBLIC_KEY_PEM =
    "-----BEGIN PUBLIC KEY-----\n"
    "_PASTE_YOUR_PROJECT_PUBLIC_KEY_BODY_HERE_\n"
    "-----END PUBLIC KEY-----\n";

// During key rotation, pin the old and the new key side by side (at most
// two), each with its key id: {"key-2027", NEW_KEY_PEM}.
static const simpleota_signing_key_t SIGNING_KEYS[] = {
    {nullptr, SIGNING_PUBLIC_KEY_PEM},
};

static bool wifiUp(void*) {
    return WiFi.status() == WL_CONNECTED;
}

static void onOtaEvent(const simpleota_event_t* e, void*) {
    if (e->id != SIMPLEOTA_EVENT_UPDATE_FAILED) return;
    if (e->reason && strcmp(e->reason, "signature_invalid") == 0) {
        // No strict signature, or one that does not match this project,
        // this build number or this image. The log says whether it was
        // missing or did not verify.
        Serial.println("[app] OTA rejected: signature invalid!");
    } else {
        Serial.printf("[app] OTA failed: %s\n", e->reason ? e->reason : "?");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println(FIRMWARE_VERSION);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.println("[app] Wi-Fi starting; the OTA task waits for an IP.");

    simpleota_config_t cfg = {};
    cfg.token = SOTA_TOKEN;
    cfg.version_label = FIRMWARE_VERSION;
    cfg.security_mode = SIMPLEOTA_SECURITY_STRICT;
    cfg.project_id = PROJECT_ID;
    cfg.signing_keys = SIGNING_KEYS;
    cfg.num_signing_keys = sizeof(SIGNING_KEYS) / sizeof(SIGNING_KEYS[0]);
    cfg.check_interval_s = 3600;
    cfg.is_connected = wifiUp;
    cfg.event_cb = onOtaEvent;

    // Strict signing fails init without a key that parses or without a
    // project id in UUID form, rather than run unprotected.
    if (simpleota_init(&cfg) != ESP_OK || simpleota_start() != ESP_OK) {
        while (true) {
            Serial.println("[app] FATAL: SimpleOTA did not start. Are "
                           "PROJECT_ID and SIGNING_PUBLIC_KEY_PEM your "
                           "project's? OTA is disabled.");
            delay(5000);
        }
    }
}

void loop() {
    // Your application code runs here; updates happen in the background.
    delay(1000);
}
