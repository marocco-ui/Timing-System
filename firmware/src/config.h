#pragma once
// Pin map and tuning for the timing box.
// Raw GPIO numbers for the Seeed XIAO ESP32-S3, with the XIAO pin label beside each.

// --- Pins ---------------------------------------------------------------
// Beam sensor output (NPN open collector). D0 = GPIO1.
// Add a 10k pull-up from D0 to 3V3. Never connect the sensor's 12 V to D0.
#define PIN_BEAM        1    // D0

// GPS PPS (pulse per second) output. D1 = GPIO2.
#define PIN_GPS_PPS     2    // D1

// GPS UART. GPS TX -> D7 (GPIO44), GPS RX -> D6 (GPIO43).
#define PIN_GPS_RX      44   // D7
#define PIN_GPS_TX      43   // D6
#define GPS_BAUD        9600

// On-board user LED (GPIO21), lit when driven LOW.
#define PIN_LED         21
#define LED_ON          LOW
#define LED_OFF         HIGH

// --- Beam trigger -----------------------------------------------------
// NPN sensors pull the line LOW when they switch. If your sensor switches
// when the beam is restored instead of broken, flip its Light-ON / Dark-ON
// setting (or change this to RISING).
#define TRIP_EDGE       FALLING

// Ignore re-trips for this long after an accepted trip. One bike breaks the
// beam several times (wheel, frame, legs).
#define LOCKOUT_US      2000000LL

// --- GPS sync -----------------------------------------------------------
// A time sentence is paired with the most recent PPS edge only if it
// arrives within this window after the edge.
#define PPS_PAIR_WINDOW_US   950000LL

// Trips more than this far from the last good GPS anchor are flagged as
// unsynced (the box is coasting on its own crystal).
#define MAX_SYNC_AGE_MS      3000

// --- Upload ---------------------------------------------------------------
#define HEARTBEAT_MS         15000
#define UPLOAD_RETRY_MS      2000
#define HTTP_TIMEOUT_MS      5000
