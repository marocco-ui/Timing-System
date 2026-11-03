#pragma once
// Copy this file to secrets.h (same folder) and fill it in.
// secrets.h is in .gitignore so keys and passwords stay out of git.

// Phone hotspots the boxes can join. List both phones so either box can use
// either hotspot. Leave unused entries as "".
#define WIFI_SSID_1     "Starter Phone"
#define WIFI_PASS_1     "changeme"
#define WIFI_SSID_2     "Finish Phone"
#define WIFI_PASS_2     "changeme"

// Supabase project: Settings > API in the Supabase dashboard.
#define SUPABASE_URL      "https://YOUR-PROJECT.supabase.co"
#define SUPABASE_ANON_KEY "YOUR-ANON-KEY"
