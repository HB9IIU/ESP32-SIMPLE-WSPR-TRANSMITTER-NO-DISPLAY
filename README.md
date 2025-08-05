# ESP32 WSPR Transmitter ⏱️📡  
**WORK IN PROGRESS — experimental code for WSPR transmission on ESP32 with Si5351 + GNSS time/location support**

This project uses an **ESP32**, a **Si5351 clock generator**, and optional **GNSS module** to transmit **WSPR (Weak Signal Propagation Reporter)** beacons with accurate time and frequency synchronization.

The app includes a web interface, automatic Maidenhead locator detection via GNSS, NTP fallback, and full WSPR symbol encoding/transmission on CLK0.

---

## ✨ Features

| Feature                          | Status       |
|----------------------------------|--------------|
| Si5351 WSPR symbol transmission  | ✅ Working    |
| GNSS Time Sync (UBX/NMEA)        | ✅ Working    |
| Automatic Locator from GNSS      | ✅ Working    |
| Maidenhead + Callsign encoder    | ✅ Working    |
| Fallback to SNTP (NTP) time      | ✅ Working    |
| Randomized sub-band frequency    | ✅ Working    |
| Web interface via SPIFFS         | ✅ Basic UI   |
| Band selection / scheduler       | 🧪 In progress|

---

## 🧠 How it works

- At startup, the ESP32 connects to Wi-Fi.
- It attempts to sync time and location from a connected **GNSS receiver** (e.g. u-blox).
- If GNSS time sync fails, it falls back to **NTP time via SNTP**.
- The **Maidenhead grid locator** is computed from GNSS latitude/longitude.
- A WSPR message is encoded from callsign + locator + TX power using `JTEncode`.
- The Si5351 outputs a 2-minute sequence of frequency-shifted tones to CLK0.
- After each TX, it powers down the Si5351 and schedules the next slot.

---

## 🛰️ GNSS Integration

- GNSS is read using UART1 and manually parsed (`$GPRMC`, `$GPGGA`, etc.)
- Time is extracted and converted to POSIX timestamp
- Coordinates are translated to **6-character Maidenhead locator**
- UTC time and locator are saved for use in WSPR messages

---

## ⏰ Time Sync Fallback

If `syncTimeFromGPS()` fails (e.g. no fix), the ESP32 uses:
```cpp
configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
