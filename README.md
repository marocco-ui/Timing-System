# Bike Builders race timing system

Two boxes time a run from a beam at the start to a beam at the finish. Each box locks its clock to GPS, so the boxes never have to talk to each other. Every beam trip goes to a Supabase database through the phone hotspot at that end, and a web app pairs starts with finishes and shows bib numbers and times.

```
platformio.ini  build settings for both boxes
firmware/    ESP32 code for both boxes (PlatformIO, VS Code)
supabase/    database tables (schema.sql)
app/         results web app (index.html, runs on any phone)
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

## Results app

`app/index.html` is the whole app: one file, no install. It works on iPhone and Android.

| Tab | Who uses it | What it does |
|---|---|---|
| Start | Starter | Type a bib, tap Arm, send the rider. Shows who's on course with a running clock. |
| Results | Anyone | Times sorted fastest first, best run per rider or all runs. CSV export. |
| Fix | Finish volunteer | Attach beam trips nobody claimed, swap finishes if a rider got passed, mark DNF. |
| Setup | Once per phone | Supabase URL and publishable key, or a share link that fills them in. |

Pairing happens in the database (`002_runs.sql`), so it works even if nobody has the app open:

1. Arming a bib adds a run.
2. The next start beam trip goes to the oldest armed run.
3. The next finish beam trip goes to the rider who started earliest and is still on course.

If a rider passes another, the finish volunteer taps the result and swaps the two finish times.

**Try it without hardware:** open the app and tap **Try demo** in Setup. The Fix tab gets buttons that fake beam trips.

### Hosting it (free, GitHub Pages)

1. On GitHub: repo **Settings > Pages**. Under Branch pick `main` and `/ (root)`, then Save.
2. After a minute the app is at `https://marocco-ui.github.io/Timing-System/app/`.
3. Open it, enter the Supabase URL and publishable key in Setup, then copy the share link from Setup and send it to the crew.

The publishable key is meant to be public, so it's fine in the share link. The repo never stores it.

## One-time setup

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE** extension.
2. In VS Code: File > Open Folder > `Timing System` (the top folder, where `platformio.ini` is). PlatformIO downloads the ESP32 tools the first time (a few minutes).
3. Copy `firmware/include/secrets.example.h` to `firmware/include/secrets.h`. Fill in both phone hotspot names and passwords, plus the Supabase URL and anon key.
4. Make a free project at [supabase.com](https://supabase.com). In SQL Editor, run `supabase/schema.sql`, then `supabase/002_runs.sql`. Both are safe to run again.
5. Use the **publishable key** (`sb_publishable_...`) from Project Settings > API Keys. Never use the secret key.

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
