// Race timing box firmware: start box A or finish box B.
//
// What it does
//   1. Locks its clock to GPS: each PPS edge is timestamped in an interrupt,
//      then paired with the UTC second from the next NMEA time sentence.
//   2. Timestamps every beam trip in an interrupt, converts it to UTC
//      microseconds, and appends it to /trips.csv on flash.
//   3. A background task joins a phone hotspot and posts each trip to
//      Supabase, oldest first. Trips survive Wi-Fi drops and reboots because
//      the flash log is the upload queue.
//
// Serial commands (115200 baud): status, dump, test

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <TinyGPSPlus.h>
#include <deque>
#include <inttypes.h>
#include "esp_timer.h"

#include "config.h"
#include "timekeeping.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and fill it in."
#endif

#ifndef BOX_ROLE
#error "BOX_ROLE is not set. Build with the start or finish environment."
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const char *TRIP_LOG = "/trips.csv";

// ---------------------------------------------------------------------------
// Interrupt-side state
// ---------------------------------------------------------------------------
static portMUX_TYPE isrMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t isrPpsUs = 0;
static volatile uint32_t isrPpsCount = 0;
static volatile int64_t isrLastTripUs = -LOCKOUT_US;

static const uint32_t TRIP_RING = 16;
static volatile int64_t tripRing[TRIP_RING];
static volatile uint32_t tripHead = 0;   // written by ISR
static volatile uint32_t tripTail = 0;   // written by loop
static volatile uint32_t tripsDropped = 0;

void IRAM_ATTR onPps() {
  int64_t t = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&isrMux);
  isrPpsUs = t;
  isrPpsCount++;
  portEXIT_CRITICAL_ISR(&isrMux);
}

void IRAM_ATTR onBeam() {
  int64_t t = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&isrMux);
  if (t - isrLastTripUs >= LOCKOUT_US) {
    isrLastTripUs = t;
    uint32_t next = (tripHead + 1) % TRIP_RING;
    if (next != tripTail) {
      tripRing[tripHead] = t;
      tripHead = next;
    } else {
      tripsDropped++;
    }
  }
  portEXIT_CRITICAL_ISR(&isrMux);
}

static bool popTrip(int64_t &localUs) {
  bool got = false;
  portENTER_CRITICAL(&isrMux);
  if (tripTail != tripHead) {
    localUs = tripRing[tripTail];
    tripTail = (tripTail + 1) % TRIP_RING;
    got = true;
  }
  portEXIT_CRITICAL(&isrMux);
  return got;
}

static void injectTrip(int64_t localUs) {  // for the "test" serial command
  portENTER_CRITICAL(&isrMux);
  uint32_t next = (tripHead + 1) % TRIP_RING;
  if (next != tripTail) {
    tripRing[tripHead] = localUs;
    tripHead = next;
  }
  portEXIT_CRITICAL(&isrMux);
}

// ---------------------------------------------------------------------------
// Shared state between the main loop and the uploader task
// ---------------------------------------------------------------------------
struct Trip {
  uint32_t seq;
  int64_t utcUs;      // 0 if the box had no GPS anchor yet
  int32_t syncAgeMs;  // -1 if no anchor
  bool synced;
  int64_t localUs;
};

struct Status {
  bool gpsLocked;
  uint32_t sats;
  int32_t anchorAgeMs;
  uint32_t pending;
};

static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
static Status status = {false, 0, -1, 0};

static QueueHandle_t tripQueue;
static std::deque<Trip> bootPending;  // handed to the uploader at start
static String boxId;

// ---------------------------------------------------------------------------
// GPS anchor
// ---------------------------------------------------------------------------
static TinyGPSPlus gps;
static Anchor anchor = {false, 0, 0};
static uint32_t consistentAnchors = 0;  // locked once this reaches 3
static const uint32_t LOCK_AFTER = 3;

static bool gpsLocked() { return anchor.valid && consistentAnchors >= LOCK_AFTER; }

static void pollGps() {
  while (Serial1.available()) {
    if (!gps.encode((char)Serial1.read())) continue;

    // A full sentence just arrived. Only act on a fresh, valid, whole-second
    // time from a receiver that has a position fix.
    if (!gps.time.isUpdated()) continue;
    // Read the values now: this clears TinyGPSPlus's "updated" flag, so a
    // stale time can never be paired with a later PPS edge.
    uint32_t tv = gps.time.value();  // hhmmsscc
    uint32_t dv = gps.date.value();  // ddmmyy
    if (!gps.time.isValid() || !gps.date.isValid()) continue;
    if (!gps.location.isValid() || gps.location.age() > 2000) continue;
    if (tv % 100 != 0) continue;  // centiseconds must be .00

    int64_t nowUs = esp_timer_get_time();
    int64_t ppsUs;
    uint32_t ppsCount;
    portENTER_CRITICAL(&isrMux);
    ppsUs = isrPpsUs;
    ppsCount = isrPpsCount;
    portEXIT_CRITICAL(&isrMux);

    // The sentence describes the second marked by the PPS edge just before it.
    if (ppsCount == 0) continue;
    int64_t sincePps = nowUs - ppsUs;
    if (sincePps < 0 || sincePps > PPS_PAIR_WINDOW_US) continue;

    int64_t epoch = utcToEpoch(2000 + dv % 100, (dv / 100) % 100, dv / 10000,
                               tv / 1000000, (tv / 10000) % 100, (tv / 100) % 100);

    if (anchor.valid && ppsUs == anchor.ppsLocalUs) continue;  // same edge (RMC and GGA)

    // Consistency check: elapsed local time between anchors should match
    // elapsed UTC seconds. A mismatch means a missed or doubled pairing.
    if (anchor.valid) {
      int64_t localSec = (ppsUs - anchor.ppsLocalUs + 500000) / 1000000;
      if (localSec == epoch - anchor.epochSec) {
        if (consistentAnchors < LOCK_AFTER) consistentAnchors++;
      } else {
        Serial.printf("[gps] anchor jump: local %" PRId64 " s vs utc %" PRId64 " s, relocking\n",
                      localSec, epoch - anchor.epochSec);
        consistentAnchors = 0;
      }
    }
    anchor = {true, ppsUs, epoch};
  }

  int64_t ageMs = anchor.valid ? (esp_timer_get_time() - anchor.ppsLocalUs) / 1000 : -1;
  portENTER_CRITICAL(&statusMux);
  status.gpsLocked = gpsLocked() && ageMs >= 0 && ageMs <= MAX_SYNC_AGE_MS;
  status.sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  status.anchorAgeMs = (int32_t)ageMs;
  portEXIT_CRITICAL(&statusMux);
}

// ---------------------------------------------------------------------------
// Flash log (also the upload queue)
// ---------------------------------------------------------------------------
static Preferences tripPrefs;  // namespace "trip": next sequence number
static uint32_t nextSeq = 1;

static bool parseTripLine(const String &line, Trip &t) {
  // seq,utc_us,sync_age_ms,synced,local_us
  const char *p = line.c_str();
  char *end;
  t.seq = strtoul(p, &end, 10);       if (*end != ',') return false; p = end + 1;
  t.utcUs = strtoll(p, &end, 10);     if (*end != ',') return false; p = end + 1;
  t.syncAgeMs = strtol(p, &end, 10);  if (*end != ',') return false; p = end + 1;
  t.synced = strtol(p, &end, 10) != 0; if (*end != ',') return false; p = end + 1;
  t.localUs = strtoll(p, &end, 10);
  return t.seq > 0;
}

static void loadPending(uint32_t lastAcked) {
  File f = LittleFS.open(TRIP_LOG, "r");
  if (!f) return;
  uint32_t maxSeq = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    Trip t;
    if (!parseTripLine(line, t)) continue;
    if (t.seq > maxSeq) maxSeq = t.seq;
    if (t.seq > lastAcked) bootPending.push_back(t);
  }
  f.close();
  if (maxSeq + 1 > nextSeq) nextSeq = maxSeq + 1;  // in case NVS was wiped
  Serial.printf("[log] %u trips waiting to upload\n", (unsigned)bootPending.size());
}

static void appendTrip(const Trip &t) {
  File f = LittleFS.open(TRIP_LOG, "a");
  if (!f) {
    Serial.println("[log] could not open trip log");
    return;
  }
  f.printf("%" PRIu32 ",%" PRId64 ",%" PRId32 ",%d,%" PRId64 "\n",
           t.seq, t.utcUs, t.syncAgeMs, t.synced ? 1 : 0, t.localUs);
  f.close();
}

static uint32_t tripFlashUntil = 0;

static void processTrips() {
  int64_t localUs;
  while (popTrip(localUs)) {
    Trip t;
    t.seq = nextSeq++;
    tripPrefs.putUInt("next", nextSeq);
    t.localUs = localUs;
    if (anchor.valid) {
      t.utcUs = eventUtcUs(anchor, localUs);
      t.syncAgeMs = syncAgeMs(anchor, localUs);
      t.synced = gpsLocked() && t.syncAgeMs <= MAX_SYNC_AGE_MS;
    } else {
      t.utcUs = 0;
      t.syncAgeMs = -1;
      t.synced = false;
    }
    appendTrip(t);
    if (xQueueSend(tripQueue, &t, 0) != pdTRUE) {
      Serial.println("[log] upload queue full; trip is on flash and uploads after reboot");
    }
    Serial.printf("[trip] #%" PRIu32 " utc_us=%" PRId64 " sync_age=%" PRId32 "ms %s\n",
                  t.seq, t.utcUs, t.syncAgeMs, t.synced ? "SYNCED" : "UNSYNCED");
    tripFlashUntil = millis() + 300;
  }
}

// ---------------------------------------------------------------------------
// Uploader task (runs on core 0, away from the GPS parser)
// ---------------------------------------------------------------------------
static bool postJson(WiFiClientSecure &tls, const String &path, const String &body,
                     const char *prefer, int &code) {
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(tls, String(SUPABASE_URL) + path)) {
    code = -1;
    return false;
  }
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  // Legacy anon keys are JWTs ("eyJ...") and also go in the Authorization
  // header. New publishable keys (sb_publishable_...) must only go in apikey.
  if (strncmp(SUPABASE_ANON_KEY, "eyJ", 3) == 0) {
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", prefer);
  code = http.POST(body);
  http.end();
  return code >= 200 && code < 300;
}

static String tripJson(const Trip &t) {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"box_id\":\"%s\",\"role\":\"%s\",\"seq\":%" PRIu32 ",\"utc_us\":%" PRId64
           ",\"sync_age_ms\":%" PRId32 ",\"synced\":%s,\"local_us\":%" PRId64 "}",
           boxId.c_str(), BOX_ROLE, t.seq, t.utcUs, t.syncAgeMs,
           t.synced ? "true" : "false", t.localUs);
  return String(buf);
}

static void uploaderTask(void *) {
  WiFiMulti wifi;
  if (strlen(WIFI_SSID_1)) wifi.addAP(WIFI_SSID_1, WIFI_PASS_1);
  if (strlen(WIFI_SSID_2)) wifi.addAP(WIFI_SSID_2, WIFI_PASS_2);

  WiFiClientSecure tls;
  tls.setInsecure();  // skips certificate checks; see README

  Preferences upl;
  upl.begin("upl", false);

  std::deque<Trip> pending;
  pending.swap(bootPending);

  uint32_t lastHeartbeat = 0;
  uint32_t retryAfter = 0;

  for (;;) {
    Trip t;
    while (xQueueReceive(tripQueue, &t, 0) == pdTRUE) pending.push_back(t);
    portENTER_CRITICAL(&statusMux);
    status.pending = pending.size();
    portEXIT_CRITICAL(&statusMux);

    if (wifi.run(5000) != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!pending.empty() && millis() >= retryAfter) {
      int code;
      const Trip &front = pending.front();
      bool ok = postJson(tls, "/rest/v1/trips?on_conflict=box_id,seq", tripJson(front),
                         "resolution=ignore-duplicates,return=minimal", code);
      if (ok || code == 409) {
        upl.putUInt("acked", front.seq);
        Serial.printf("[upload] trip #%" PRIu32 " sent (%d)\n", front.seq, code);
        pending.pop_front();
      } else if (code >= 400 && code < 500 && code != 408 && code != 429) {
        // The server rejected this row outright. Retrying won't help; it stays in the flash log.
        Serial.printf("[upload] trip #%" PRIu32 " rejected (%d), skipping\n", front.seq, code);
        upl.putUInt("acked", front.seq);
        pending.pop_front();
      } else {
        Serial.printf("[upload] trip #%" PRIu32 " failed (%d), retrying\n", front.seq, code);
        retryAfter = millis() + UPLOAD_RETRY_MS;
      }
    }

    if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
      lastHeartbeat = millis();
      Status s;
      portENTER_CRITICAL(&statusMux);
      s = status;
      portEXIT_CRITICAL(&statusMux);
      char buf[256];
      snprintf(buf, sizeof(buf),
               "{\"box_id\":\"%s\",\"role\":\"%s\",\"gps_locked\":%s,\"sats\":%" PRIu32
               ",\"anchor_age_ms\":%" PRId32 ",\"pending\":%" PRIu32 ",\"rssi\":%d,\"fw\":\"%s\"}",
               boxId.c_str(), BOX_ROLE, s.gpsLocked ? "true" : "false", s.sats,
               s.anchorAgeMs, s.pending, (int)WiFi.RSSI(), FW_VERSION);
      int code;
      postJson(tls, "/rest/v1/box_status?on_conflict=box_id", String(buf),
               "resolution=merge-duplicates,return=minimal", code);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ---------------------------------------------------------------------------
// Status LED
//   fast blink        no GPS lock yet (wait outside with sky view)
//   double blink      GPS locked, not on Wi-Fi
//   short blink / 1 s GPS locked and online, ready
//   solid 0.3 s       trip recorded
// ---------------------------------------------------------------------------
static void updateLed() {
  uint32_t now = millis();
  bool on;
  bool locked;
  portENTER_CRITICAL(&statusMux);
  locked = status.gpsLocked;
  portEXIT_CRITICAL(&statusMux);
  bool online = WiFi.status() == WL_CONNECTED;

  if (now < tripFlashUntil) {
    on = true;
  } else if (!locked) {
    on = (now % 250) < 125;
  } else if (!online) {
    uint32_t ph = now % 2000;
    on = ph < 100 || (ph >= 250 && ph < 350);
  } else {
    on = (now % 1000) < 60;
  }
  digitalWrite(PIN_LED, on ? LED_ON : LED_OFF);
}

// ---------------------------------------------------------------------------
// Serial commands
// ---------------------------------------------------------------------------
static void handleSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "status") {
    Status s;
    portENTER_CRITICAL(&statusMux);
    s = status;
    portEXIT_CRITICAL(&statusMux);
    Serial.printf("box=%s role=%s fw=%s\n", boxId.c_str(), BOX_ROLE, FW_VERSION);
    Serial.printf("gps_locked=%d sats=%" PRIu32 " anchor_age_ms=%" PRId32 " pps_count=%" PRIu32 "\n",
                  s.gpsLocked, s.sats, s.anchorAgeMs, (uint32_t)isrPpsCount);
    Serial.printf("wifi=%s rssi=%d pending=%" PRIu32 " next_seq=%" PRIu32 " dropped=%" PRIu32 "\n",
                  WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "offline",
                  (int)WiFi.RSSI(), s.pending, nextSeq, (uint32_t)tripsDropped);
  } else if (cmd == "dump") {
    File f = LittleFS.open(TRIP_LOG, "r");
    Serial.println("seq,utc_us,sync_age_ms,synced,local_us");
    while (f && f.available()) Serial.write(f.read());
    if (f) f.close();
  } else if (cmd == "test") {
    injectTrip(esp_timer_get_time());
    Serial.println("fake trip injected");
  } else if (cmd.length()) {
    Serial.println("commands: status, dump, test");
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1500);  // give USB serial a moment to connect
  Serial.printf("\nTiming box %s, firmware %s\n", BOX_ROLE, FW_VERSION);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LED_OFF);

  if (!LittleFS.begin(true)) Serial.println("[log] LittleFS mount failed");

  tripPrefs.begin("trip", false);
  nextSeq = tripPrefs.getUInt("next", 1);
  {
    Preferences upl;
    upl.begin("upl", true);
    uint32_t lastAcked = upl.getUInt("acked", 0);
    upl.end();
    loadPending(lastAcked);
  }

  WiFi.mode(WIFI_STA);
  String mac = WiFi.macAddress();  // "AA:BB:CC:DD:EE:FF"
  mac.replace(":", "");
  boxId = String(BOX_ROLE) + "-" + mac.substring(8);
  Serial.printf("box id %s\n", boxId.c_str());

  Serial1.setRxBufferSize(1024);
  Serial1.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  pinMode(PIN_GPS_PPS, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_GPS_PPS), onPps, RISING);

  pinMode(PIN_BEAM, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BEAM), onBeam, TRIP_EDGE);

  tripQueue = xQueueCreate(64, sizeof(Trip));
  xTaskCreatePinnedToCore(uploaderTask, "upload", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  pollGps();
  processTrips();
  updateLed();
  handleSerial();
  delay(1);
}
