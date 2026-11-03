# Bike Builders race timing system

Two boxes time a run from a beam at the start to a beam at the finish. Each box locks its clock to GPS, so the boxes never have to talk to each other. Every beam trip goes to a Supabase database through the phone hotspot at that end, and a web app pairs starts with finishes and shows bib numbers and times.

```
platformio.ini  build settings for both boxes
firmware/    ESP32 code for both boxes (PlatformIO, VS Code)
supabase/    database tables (schema.sql)
app/         results web app (coming next)
```

## Hardware per box

| Part | Notes |
|---|---|
| Seeed XIAO ESP32-S3 | Wi-Fi, USB-C |
| NEO-6M GPS module with a PPS pin | Buy the 5-pin version (VCC, GND, TX, RX, PPS) |
| Retroreflective beam sensor, NPN output, 6 to 36 VDC | Transistor output, not relay |
| MT3608 boost module | Set to 12 V before connecting the sensor |
| 10 kΩ resistor | Pull-up on the beam signal |
| USB power bank | Powers the XIAO through USB-C |

## Wiring

| From | To |
|---|---|
| GPS VCC | XIAO 3V3 |
| GPS GND | XIAO GND |
| GPS TX | XIAO D7 (RX) |
| GPS RX | XIAO D6 (TX) |
| GPS PPS | XIAO D1 |
| XIAO 5V | Boost IN+ |
| XIAO GND | Boost IN- |
| Boost OUT+ (12 V) | Sensor brown (+V) |
| Boost OUT- | Sensor blue (0 V) **and** XIAO GND |
| Sensor black (NPN output) | XIAO D0 |
| 10 kΩ resistor | Between XIAO D0 and XIAO 3V3 |

Check the sensor's wire colors against its own label. Never put 12 V on a XIAO pin. Set the boost to 12 V with a multimeter before you connect the sensor.

## One-time setup

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE** extension.
2. In VS Code: File > Open Folder > `Timing System` (the top folder, where `platformio.ini` is). PlatformIO downloads the ESP32 tools the first time (a few minutes).
3. Copy `firmware/include/secrets.example.h` to `firmware/include/secrets.h`. Fill in both phone hotspot names and passwords, plus the Supabase URL and anon key.
4. Make a free project at [supabase.com](https://supabase.com). Open SQL Editor, paste `supabase/schema.sql`, and run it.

Free Supabase projects pause after a week with no activity. Open the dashboard the day before a ride so it's awake.

## Flashing

Plug the XIAO in over USB-C, then in the PlatformIO sidebar:

- **start > Upload** for box A
- **finish > Upload** for box B
- **Monitor** to see the serial log

From a terminal in the top folder: `pio run -e start -t upload`, then `pio device monitor`.

If upload fails, hold the XIAO's BOOT button while plugging it in, then upload again.

## LED codes

| LED | Meaning |
|---|---|
| Fast blink | No GPS lock yet. Put the box where it can see the sky. A cold start can take a few minutes. |
| Double blink every 2 s | GPS locked, not on Wi-Fi |
| Short blink every 1 s | Locked and online. Ready. |
| Solid for 0.3 s | Trip recorded |

## Serial commands

Open the monitor at 115200 baud and type:

- `status`: GPS lock, satellites, Wi-Fi, trips waiting to upload
- `dump`: print every trip stored on the box
- `test`: record a fake trip without the sensor

## How the timing works

1. The GPS PPS pin pulses at the start of every UTC second. An interrupt timestamps each pulse with the ESP32's microsecond clock.
2. The NMEA sentence that follows says which second that pulse was. The pair is the box's "anchor".
3. The box only counts itself locked after 3 anchors in a row agree with each other.
4. A beam trip is timestamped in an interrupt, then converted: UTC = anchor second + (trip time − pulse time).
5. Trips within 2 s of an accepted trip are ignored, since one bike breaks the beam several times.

Each trip is written to flash before it's uploaded, so nothing is lost if Wi-Fi drops or the box reboots. The upload picks up where it left off.

## Bench check before the first ride

1. Put both boxes outside until both show the 1 s blink.
2. Wave a hand through both beams at the same moment, or trigger both with one switch wired to both D0 pins.
3. Compare the two `utc_us` values in Supabase. They should match within about a millisecond.

If they're off by exactly 1 second, the two GPS modules report their time sentences differently. Use the same GPS model in both boxes; any whole-second quirk is then identical at both ends and cancels out of run times.

## Known limits

- `tls.setInsecure()` skips HTTPS certificate checks to keep setup simple. Traffic is still encrypted.
- The anon key in `secrets.h` lets anyone who has it add rows. Keep it out of git (it's in `.gitignore`).
