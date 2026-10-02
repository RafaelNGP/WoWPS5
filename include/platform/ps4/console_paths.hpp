#pragma once
// Where the console build keeps the user's client data and everything it
// writes. PS4: /data/wow_ps, which every homebrew title may use. PS5: a native
// app's sandbox reaches /data only after elevation, so the first PS5 builds keep
// it all inside the app folder (/app0 is /data/homebrew/<TITLE_ID>/, writable
// for a ShadowMountPlus folder title): the MPQs go to /app0/Data beside the
// app's own Data/expansions tables.
#if defined(WOWEE_PS5)
#define WOWEE_CONSOLE_DATA_ROOT "/app0"
#else
#define WOWEE_CONSOLE_DATA_ROOT "/data/wow_ps"
#endif
