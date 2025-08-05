#include <WiFi.h>
#include <LittleFS.h>         
#define FILESYSTEM LittleFS   
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "Preferences.h"
#include <si5351.h>
#include <JTEncode.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h> // Library to enable mDNS (Multicast DNS) for resolving local hostnames like "device.local"
#include <TinyGPS++.h>
#define SI5351_SDA 25
#define SI5351_SCL 26
#define GPS_RX 16  // GPS TX → ESP32 RX2
#define GPS_TX 17  // GPS RX → ESP32 TX2
#define PPS_PIN 27 // PPS → GPIO27

HardwareSerial GPSserial(2);

#define VERSION "Beta 0"
extern "C"
{
#include "esp_sntp.h"
}
// Wi-Fi credentials
const char *ssid = "NO WIFI FOR YOU!!!";
const char *password = "Nestle2010Nestle";

char call[8]; // USER CALLSIGN will be retrieved through preferences
char loc[7];  // USER MAIDENHEAD GRID LOCATOR first 6 letters.
uint32_t power_mW;
uint8_t dbm = 0;

// Create the Si5351 object
Si5351 si5351;
// Create the jtencode object
JTEncode jtencode;

// Create the TinyGPSPlus object
TinyGPSPlus gps;

// Create a Preferences object for storing and retrieving key-value
Preferences preferences;

// Calibration variables
int32_t cal_factor = 0;
int calFrequencyInMhz = 0;

bool warmingup = false;
unsigned long long WSPR_TX_operatingFrequ;
unsigned long long TX_referenceFrequ = 0;
TaskHandle_t txCounterTaskHandle = NULL;
bool gpsTimeSynced = false; // becomes true if GPS succeeds
unsigned long lastGPSretry = 0;
const unsigned long gpsRetryInterval = 15 * 60 * 1000; // 15 minutes
time_t lastManualSync = 0;                             // Last time we did a manual sync
const time_t manualSyncInterval = 15 * 60;             // Sync every 15 minutes

// Timing variables
volatile bool isFirstIteration = true;
volatile bool interruptWSPRcurrentTX = false;
// struct tm timeinfo;
time_t currentEpochTime;
time_t nextPosixTxTime;
time_t currentRemainingSeconds;
time_t intervalBetweenTx = 120;

int modeOfOperation = 2;    // inherited from MLA toolbox
byte selectedBandIndex = 3; // inherited from MLA toolbox
unsigned long now;
// TX status
bool tx_is_ON = false;
int tx_ON_running_time_in_s = 0;

#define TONE_SPACING 146 // ~1.46 Hz
#define WSPR_DELAY 683   // Delay value for WSPR
#define WSPR_CTC 10672   // CTC value for WSPR
#define SYMBOL_COUNT WSPR_SYMBOL_COUNT

#define SEND_INTV 10
#define RECV_TIMEOUT 10

#define SI5351_REF 25000000UL // si5351’s crystal frequency, 25 Mhz or 27 MHz
uint8_t tx_buffer[SYMBOL_COUNT];

// Async web server runs on port 80
AsyncWebServer server(80);

// prototypes

void initSI5351();
String convertPosixToHHMMSS(time_t posixTime);
void si5351_WarmingUp();
void transmitWSPR();

String formatFrequencyWithDots(unsigned freq);

void manuallyResyncTime();
void initialTimeSynViaSNTP();
bool syncTimeFromGPS(unsigned long timeoutMs = 5000);
String latLonToMaidenhead(float lat, float lon);
void wsprTXTask(void *parameter);
// ################################################################################################
// Prototype declarations
// related to WSPR
// ✅ WSPR Band Definitions (Hz) from official sub-band plan
// https://www.wsprnet.org/drupal/sites/wsprnet.org/files/wspr-qrg.pdf
const char *WSPRbandNames[] = {
    "80m", "40m", "30m", "20m", "17m", "15m", "12m", "10m"};

const unsigned long WSPRbandStart[] = {
    3570000, 7040000, 10140100, 14097000, 18106000, 21096000, 24926000, 28126000};

const unsigned long WSPRbandEnd[] = {
    3570200, 7040200, 10140300, 14097200, 18106200, 21096200, 24926200, 28126200};

const byte numWSPRbands = sizeof(WSPRbandNames) / sizeof(WSPRbandNames[0]);

// ✅ Returns a randomized safe WSPR transmit frequency for a given band index
unsigned long setRandomWSPRfrequency(byte bandIndex);
void displaySelecetdBandInformation(byte bandIndex);

void initializeNextTransmissionTime();
void retrieveUserSettings();

void configure_web_server();
void setFrequencyInMhz(float freqMHz);
//----------------------------------------------------------------------------------------

//---------------------------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);
    delay(4000);

    // Initialize LittleFS
    if (!FILESYSTEM.begin(true))
    {
        Serial.println("An error occurred while mounting LittleFS");
        return;
    }
    Serial.println("LittleFS mounted successfully");
    // Connect to Wi-Fi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected to Wi-Fi");
    Serial.println(WiFi.localIP());
    // Retrieve user settings
    retrieveUserSettings();
    if (!syncTimeFromGPS())
    {
        initialTimeSynViaSNTP(); // your existing SNTP fallback
    }

    // init RF module
    initSI5351();
    // Start server

    configure_web_server();
}
//---------------------------------------------------------------------------------------------
void loop()
{

    // If interrupted, skip TX
    if (interruptWSPRcurrentTX)
    {
        // Serial.println("\n⚠️ TX Interrupted! Skipping transmission...");
        // Let background tasks (Wi-Fi, web) run
        delay(5);
        yield();
        return;
    }

    // First-time initialization
    if (isFirstIteration)
    {
        Serial.println("\n🔁 First Loop Iteration: Determining next TX start time...");
        intervalBetweenTx = 2 * 60; // 2 minutes for WSPR
        initializeNextTransmissionTime();
        isFirstIteration = false;
        interruptWSPRcurrentTX = false;
        TX_referenceFrequ = WSPRbandStart[selectedBandIndex]; // TEMPORARY
    }

    // Get current time once per loop
    currentEpochTime = time(nullptr);

    Serial.print("\n🕒 Current time: ");
    Serial.println(convertPosixToHHMMSS(currentEpochTime));
    Serial.print("🕑 Next TX Time: ");
    Serial.println(convertPosixToHHMMSS(nextPosixTxTime));

    displaySelecetdBandInformation(selectedBandIndex);

    // Countdown loop until it's time to transmit or interrupted
    unsigned long lastUpdate = 0;

    while (true)
    {
        currentEpochTime = time(nullptr);
        currentRemainingSeconds = nextPosixTxTime - currentEpochTime;

        // Break the loop if time is up or interrupted
        if (currentEpochTime >= nextPosixTxTime || interruptWSPRcurrentTX)
            break;

        // Update serial output every 1 second (not every loop)
        if (millis() - lastUpdate >= 1000)
        {
            Serial.printf("\r⏳ TX starts in %ld s   ", currentRemainingSeconds);
            lastUpdate = millis();
        }

        // Start warming up if close to TX time
        if (currentRemainingSeconds <= 5 && !warmingup)
        {
            si5351_WarmingUp();
        }

        delay(5); // Small sleep to avoid tight spin
        yield();  // Allow Wi-Fi + web tasks to run
    }

    // Transmission time reached
    Serial.println("\n🚀 Starting WSPR Transmission...");

    // Update for the next TX slot (every 2 minutes)
    nextPosixTxTime += intervalBetweenTx;
    if (interruptWSPRcurrentTX)
    {
        return; // not clean, we need to find better way
    }

    // Begin transmission
   xTaskCreatePinnedToCore(
    wsprTXTask,         // Task function
    "WSPR_TX_Task",     // Name
    4096,               // Stack size in words (larger since it includes WSPR encode + TX)
    NULL,               // Parameter
    1,                  // Priority
    NULL,               // No handle needed
    1                   // Run on Core 1
);
    // If interrupted, skip TX
    if (!interruptWSPRcurrentTX)
    {
        // Shutdown Si5351 output after TX
        si5351.set_clock_pwr(SI5351_CLK0, 0);
        Serial.println("📴 --- TX OFF: Transmission Complete ---\n");
    }

    // 🔁 Try to resync time from GPS if not yet synced
    unsigned long nowMs = millis();
    if (!gpsTimeSynced && (nowMs - lastGPSretry > gpsRetryInterval))
    {
        lastGPSretry = nowMs;
        if (syncTimeFromGPS(3000))
        {
            Serial.println("✅ Recovered time from GPS.");
        }
        else
        {
            manuallyResyncTime();
        }
    }
}

//---------------------------------------------------------------------------------------------

void initSI5351()
{
    const uint8_t SI5351_ADDRESS = 0x60;

    // 🔌 Start I²C bus at 400 kHz (fast mode) — reliable and quick on ESP32
    Wire.begin(SI5351_SDA, SI5351_SCL);
    Wire.setClock(400000);

    // 🔍 Probe Si5351 I²C address to check if device is present
    Wire.beginTransmission(SI5351_ADDRESS);
    if (Wire.endTransmission() != 0)
    {
        Serial.println("❌ Si5351 not found at 0x60. Check wiring or power.");
        return;
    }

    // ✅ Found and ready to initialize
    Serial.println("✅ Success! Si5351 module found!! 🎉");
    Serial.println("📡 Initializing Si5351...");

    // ⚙️ Initialize Si5351 with 8pF crystal load (typical for cheap boards)
    si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0); // 0, 0 = use default XTAL + correction

    // 🔧 Apply frequency calibration correction (in parts per billion)
    Serial.print("🔧 Applying Calibration Factor: ");
    Serial.println(cal_factor);
    si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);

    // 📶 Set frequency for selected TX clock output (CLK0, CLK1, or CLK2)
    Serial.print("📶 Setting TX Reference Frequency: ");
    Serial.print(TX_referenceFrequ);
    Serial.println(" Hz");
    si5351.set_freq(TX_referenceFrequ, SI5351_CLK0);

    // 💪 Set drive strength for the selected clock output (strongest = 8 mA)
    si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);

    // 📴 Disable all unused outputs among CLK0, CLK1, and CLK2
    Serial.println("🔌 Disabling unused clock outputs: ");
    bool first = true;
    si5351_clock clocks[] = {SI5351_CLK0, SI5351_CLK1, SI5351_CLK2};

    for (si5351_clock clk : clocks)
    {
        si5351.set_clock_pwr(clk, 0); // Disable unused clock output
        // Serial.print("Disabling CLK");
        // Serial.println((int)clk); // newline for better readability
    }
    Serial.println();
    // switch OFF
    si5351.set_clock_pwr(SI5351_CLK0, 0);
}
void initializeNextTransmissionTime()
{

    // 🕒 Get Current Epoch Time

    currentEpochTime = time(nullptr);
    int currentHour = (currentEpochTime % 86400L) / 3600;
    int currentMinute = (currentEpochTime % 3600) / 60;
    int currentSecond = currentEpochTime % 60;

    // 🗓️ Determine Next Even Minute
    int nextEvenMinute = (currentMinute % 2 == 0) ? currentMinute + 2 : currentMinute + 1;
    int nextHour = currentHour;

    // ⏭️ Handle Hour Rollover
    if (nextEvenMinute >= 60)
    {
        nextEvenMinute = 0;
        nextHour = (currentHour + 1) % 24;
    }

    // 📅 Calculate Next Transmission Time
    if (nextHour != currentHour)
    {
        nextPosixTxTime = currentEpochTime + ((60 - currentMinute) * 60 - currentSecond) + (nextEvenMinute * 60);
    }
    else
    {
        nextPosixTxTime = currentEpochTime + (nextEvenMinute - currentMinute) * 60 - currentSecond;
    }

    // ✅ Log the Result
    Serial.print("📅 Next Transmission Time (POSIX): ");
    Serial.println(nextPosixTxTime);
    Serial.print("🕑 Next TX Time (Human Readable):  ");
    Serial.println(convertPosixToHHMMSS(nextPosixTxTime));
}

String convertPosixToHHMMSS(time_t posixTime)
{
    // Convert epoch time to struct tm
    struct tm *timeInfo;
    timeInfo = gmtime(&posixTime); // Use localtime() for local time or gmtime() for UTC time

    // Format the time
    char formattedTime[9]; // HH:MM:SS + null terminator
    sprintf(formattedTime, "%02d:%02d:%02d", timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);

    // Return formatted time as String
    return String(formattedTime);
}

void si5351_WarmingUp()
{
    warmingup = true;
    Serial.println();
    Serial.println("🔥 Radio Module 'Warming Up' Started...");

    // 🎛️ Apply small random frequency offset (-100 to +100 Hz)

    WSPR_TX_operatingFrequ = setRandomWSPRfrequency(selectedBandIndex) * 100ULL; /// Hz * 100 for module

    // 📡 Log the new TX frequency with formatting
    Serial.print("📶 Setting TX Frequency to: ");
    Serial.print(formatFrequencyWithDots(WSPR_TX_operatingFrequ / 100ULL));
    Serial.print("     (ref. ");
    Serial.print(formatFrequencyWithDots(TX_referenceFrequ));
    Serial.println(")");
    // ⚙️ Configure Si5351 for transmission
    si5351.set_freq(WSPR_TX_operatingFrequ, SI5351_CLK0);
    si5351.set_clock_pwr(SI5351_CLK0, 1); // Power ON
}


// ✅ Returns a randomized safe WSPR transmit frequency for a given band index
unsigned long setRandomWSPRfrequency(byte bandIndex)
{
    unsigned long officialStart = WSPRbandStart[bandIndex];
    unsigned long officialEnd = WSPRbandEnd[bandIndex];
    unsigned long minF = officialStart + 3;
    unsigned long maxF = officialEnd - 3;

    unsigned long freq = random(minF, maxF + 1); // inclusive range

    Serial.printf("🎯 Selected random freq within available range:  %lu Hz\n", freq);

    return freq;
}

String formatFrequencyWithDots(unsigned freq)
{
    String s = String(freq);
    int len = s.length();

    // Insert dots every 3 digits from the right
    for (int i = len - 3; i > 0; i -= 3)
    {
        s = s.substring(0, i) + "." + s.substring(i);
    }

    return s;
}


void displaySelecetdBandInformation(byte bandIndex)
{
    unsigned long officialStart = WSPRbandStart[bandIndex];
    unsigned long officialEnd = WSPRbandEnd[bandIndex];
    unsigned long minF = officialStart + 3;
    unsigned long maxF = officialEnd - 3;

    Serial.println();
    Serial.println("📶 --- WSPR Transmission Setup --------------------------------------");

    Serial.printf("🔹 Band:            %s\n", WSPRbandNames[bandIndex]);
    Serial.printf("🔹 Sub-band:        %lu – %lu Hz\n", officialStart, officialEnd);
    Serial.printf("🔹 Usable range:    %lu – %lu Hz (excludes ±3 Hz edges)\n", minF, maxF);
    Serial.println("🔹 Bandwidth used:  ~6 Hz centered around selected frequency");
    Serial.printf("🧾 Callsign:        %s\n", call);
    Serial.printf("🌍 Locator:         %s\n", loc);
    Serial.printf("⚡ Power:           %d mW → %d dBm\n", power_mW, dbm);
    Serial.println("---------------------------------------------------------------------");
    Serial.println();
}
void transmitWSPR()
{
    uint8_t i;

    // 🎙️ Encode WSPR message
    Serial.println("\n📝 Preparing WSPR message for encoding...");
    Serial.print("📡 Callsign: ");
    Serial.println(call);

    Serial.print("🌍 Locator: ");
    Serial.println(loc);

    Serial.print("⚡ Power: ");
    Serial.print(dbm);
    Serial.println(" dBm");
    jtencode.wspr_encode(call, loc, dbm, tx_buffer);

    // 🚀 Transmission Start
    Serial.println("\n📡 --- TX ON: Starting Transmission ---");

    // 🔊 Transmit each WSPR symbol
    for (int i = 0; i < SYMBOL_COUNT; i++)
    {
        uint64_t toneFreq = WSPR_TX_operatingFrequ + (tx_buffer[i] * TONE_SPACING);
        si5351.set_freq(toneFreq, SI5351_CLK0);
        delay(WSPR_DELAY);
        yield();
        if (interruptWSPRcurrentTX == true)
        {
            break; // Exit the for loop
        }
    }

    warmingup = false;
}

void retrieveUserSettings()
{
    Serial.println("📂 Loading user settings from NVS...");

    // ✅ Open preferences namespace
    preferences.begin("settings", false);

    // 📡 Retrieve Callsign
    String storedCall = preferences.getString("callsign");
    if (storedCall.isEmpty())
    {
        Serial.println("⚠️ Callsign not found! Setting default to 'HB9IIU' 🆕");
        storedCall = "HB9IIU";
        preferences.putString("callsign", storedCall);
    }
    else
    {
        Serial.println("✅ Callsign retrieved successfully.");
    }
    storedCall.toCharArray(call, sizeof(call));
    Serial.printf("📢 Callsign: %s\n", call);

    // 🌍 Retrieve Locator
    String storedLoc = preferences.getString("locator");
    if (storedLoc.isEmpty())
    {
        Serial.println("⚠️ Locator not found! Setting default to 'XX00XX' 🆕");
        storedLoc = "XX00XX";
        preferences.putString("locator", storedLoc);
    }
    else
    {
        Serial.println("✅ Locator retrieved successfully.");
    }
    storedLoc.toCharArray(loc, sizeof(loc));
    Serial.printf("📢 Locator: %s\n", loc);

    // ⚡ Retrieve Power in mW
    power_mW = preferences.getUInt("power", 0);
    if (power_mW == 0)
    {
        Serial.println("⚠️ Power value not found! Setting default to 250 mW 🆕");
        power_mW = 250;
        preferences.putUInt("power", power_mW);
    }
    else
    {
        Serial.println("✅ Power retrieved successfully.");
    }
    dbm = round(10 * log10(power_mW));
    Serial.printf("📢 Power: %d mW → %d dBm\n", power_mW, dbm);

    // ⏲️ Retrieve Schedule State
    String scheduleState = preferences.getString("scheduleState");
    if (scheduleState.isEmpty())
    {
        Serial.println("⚠️ Schedule state not found! Setting default to 2 minutes 🆕");
        scheduleState = "schedule1";
        preferences.putString("scheduleState", scheduleState);
    }
    else
    {
        Serial.println("✅ Schedule state retrieved successfully.");
    }

    if (scheduleState == "schedule1")
        intervalBetweenTx = 2 * 60;
    else if (scheduleState == "schedule2")
        intervalBetweenTx = 4 * 60;
    else if (scheduleState == "schedule3")
        intervalBetweenTx = 6 * 60;
    else if (scheduleState == "schedule4")
        intervalBetweenTx = 8 * 60;
    else if (scheduleState == "schedule5")
        intervalBetweenTx = 10 * 60;
    else
        intervalBetweenTx = 2 * 60;

    Serial.printf("📢 Transmission Interval: %d seconds (%d minutes)\n",
                  intervalBetweenTx, intervalBetweenTx / 60);

    // 🔧 Retrieve Calibration Factor
    cal_factor = preferences.getInt("cal_factor", 9999999);
    if (cal_factor == 9999999)
    {
        Serial.println("⚠️ Calibration factor not found! Setting default to 0 🆕");
        cal_factor = 0;
        preferences.putInt("cal_factor", cal_factor);
    }
    else
    {
        Serial.println("✅ Calibration factor retrieved successfully.");
    }
    Serial.printf("📏 Calibration Factor: %d\n", cal_factor);

    // ✅ Close preferences
    preferences.end();
    Serial.println();
}

void configure_web_server()
{
    Serial.println("🌍 Starting Web Server Route Configuration...");

    server.on("/getModeOfOperation", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    DynamicJsonDocument doc(128);
    doc["modeOfOperation"] = modeOfOperation;

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    request->send(200, "application/json", jsonResponse); });

    // Root and static pages
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("📄 Route: / -> /index.html");
    request->send(FILESYSTEM, "/index.html", "text/html"); });

    server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("📄 Route: /index.html -> /index.html");
    request->send(FILESYSTEM, "/index.html", "text/html"); });

    server.on("/calibrate.html", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("🔧 Route: /calibrate.html -> /calibrate.html");
    request->send(FILESYSTEM, "/calibrate.html", "text/html"); });

    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("🔖 Route: /favicon.ico");
    request->send(FILESYSTEM, "/favicon.ico", "image/x-icon"); });

    // Static assets
    server.on("/assets/spectrum.jpg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("🖼️ Route: /assets/spectrum.jpg");
    request->send(FILESYSTEM, "/assets/spectrum.jpg", "image/jpeg"); });

    server.on("/assets/wsprlogo.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("🖼️ Route: /assets/wsprlogo.png");
    request->send(FILESYSTEM, "/assets/wsprlogo.png", "image/png"); });
    server.on("/assets/warning.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("🖼️ Route: /assets/warning.png");
    request->send(FILESYSTEM, "/assets/warning.png", "image/png"); });

    server.on("/cesium.key", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(FILESYSTEM, "/cesium.key", "text/plain"); });

    // 🔧 Settings and data endpoints
    server.on("/getAllSettings", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    preferences.begin("settings", true);
    StaticJsonDocument<256> doc;
    doc["version"] = "Ver. " + String(VERSION);
    doc["callsign"] = preferences.getString("callsign", "");
    doc["locator"] = preferences.getString("locator", "");
    doc["power"] = preferences.getUInt("power", 24);
    doc["TX_referenceFrequ"] = TX_referenceFrequ;
    doc["WSPR_TX_operatingFrequ"] = WSPR_TX_operatingFrequ;
    doc["scheduleState"] = preferences.getString("scheduleState", "schedule1");
    preferences.end();

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json); });

    server.on("/getLocator", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    preferences.begin("settings", true);
    String locator = preferences.getString("locator", "");
    preferences.end();

    Serial.println("📤 Sending Locator...");
    StaticJsonDocument<64> doc;
    doc["locator"] = locator;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json); });

    server.on("/getPower", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    preferences.begin("settings", true);
    uint32_t power = preferences.getUInt("power", 24);
    preferences.end();

    Serial.println("📤 Sending Power Info...");
    StaticJsonDocument<64> doc;
    doc["power"] = power;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json); });

    server.on("/getTimes", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    //Serial.printf("📤 Sending WSPR Timing Info to web page (TX = %d s, Next = %d s)\n", tx_ON_running_time_in_s, currentRemainingSeconds);
    StaticJsonDocument<128> doc;
    doc["currentRemainingSeconds"] = currentRemainingSeconds;
    doc["txRunningTime"] = tx_ON_running_time_in_s;
    doc["TX_referenceFrequ"] = TX_referenceFrequ ;
    doc["intervalBetweenTx"] = intervalBetweenTx;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json); });

    // 🔄 Settings update routes
    server.on("/updateCallsign", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    if (request->hasParam("callsign")) {
      String callsign = request->getParam("callsign")->value();
      preferences.begin("settings", false);
      preferences.putString("callsign", callsign);
      preferences.end();
      Serial.printf("📡 Callsign set to %s\n", callsign.c_str());
    }
    request->send(200, "text/plain", "Callsign updated"); });

    server.on("/updateLocator", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    if (request->hasParam("locator")) {
      String locator = request->getParam("locator")->value();
      preferences.begin("settings", false);
      preferences.putString("locator", locator);
      preferences.end();
      Serial.printf("📍 Locator set to %s\n", locator.c_str());
    }
    request->send(200, "text/plain", "Locator updated"); });

    server.on("/updatePower", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    if (request->hasParam("power")) {
      String power = request->getParam("power")->value();
      preferences.begin("settings", false);
      preferences.putUInt("power", power.toInt());
      preferences.end();
      Serial.printf("⚡ Power set to %s mW\n", power.c_str());
    }
    request->send(200, "text/plain", "Power updated"); });

    server.on("/updateScheduleState", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    if (request->hasParam("id")) {
      String scheduleState = request->getParam("id")->value();
      preferences.begin("settings", false);
      preferences.putString("scheduleState", scheduleState);
      preferences.end();

      if (scheduleState == "schedule1") intervalBetweenTx = 2 * 60;
      else if (scheduleState == "schedule2") intervalBetweenTx = 4 * 60;
      else if (scheduleState == "schedule3") intervalBetweenTx = 6 * 60;
      else if (scheduleState == "schedule4") intervalBetweenTx = 8 * 60;
      else if (scheduleState == "schedule5") intervalBetweenTx = 10 * 60;
      else intervalBetweenTx = 2 * 60;

      Serial.printf("📅 New schedule selected: %s ➡️ Interval set to %d minutes\n", scheduleState.c_str(), intervalBetweenTx / 60);
      isFirstIteration = true;
      interruptWSPRcurrentTX = true;
    } else {
      Serial.println("⚠️ No schedule ID received!");
    }
    request->send(200, "text/plain", "OK"); });

    // 🛠️ Control and calibration
    server.on("/setFrequency", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("🛠️ Changing mode of operation to: Analyze (sweeper)");
    interruptWSPRcurrentTX = true;
    if (request->hasParam("frequency")) {
      String frequencyStr = request->getParam("frequency")->value();
      calFrequencyInMhz = strtoull(frequencyStr.c_str(), NULL, 10);
      setFrequencyInMhz(calFrequencyInMhz);
      Serial.printf("📡 Frequency set to %s Hz and clock powered ON.\n", formatFrequencyWithDots(calFrequencyInMhz * 1e6).c_str());
    }
    request->send(200, "text/plain", "Frequency and clock power set"); });

    server.on("/updateCalFactor", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    if (request->hasParam("calFactor")) {
      cal_factor = request->getParam("calFactor")->value().toInt();
      si5351.set_correction(cal_factor, SI5351_PLL_INPUT_XO);
      Serial.printf("📏 Calibration factor set to %d\n", cal_factor);
    }
    request->send(200, "text/plain", "Calibration factor updated"); });

    server.on("/saveCalFactor", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    modeOfOperation = 2;
    isFirstIteration = true;
    if (request->hasParam("calFactor")) {
      cal_factor = request->getParam("calFactor")->value().toInt();
      preferences.begin("settings", false);
      preferences.putInt("cal_factor", cal_factor);
      preferences.end();
      Serial.printf("\n📏 Calibration factor saved: %d\n", cal_factor);
      si5351.set_clock_pwr(SI5351_CLK0, 0);  // TX OFF
      Serial.println("✅ Exiting Calibration Mode, WSPR TX Resumed.");
    }
    request->send(200, "text/plain", "Calibration factor saved"); });

    server.on("/getCalFactor", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.printf("📤 Sending Calibration Factor: %d\n", cal_factor);
    request->send(200, "text/plain", String(cal_factor)); });

    // Reboot and reset
    server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("🔄 Reboot requested...");
    request->send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart(); });

    server.on("/factoryReset", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    Serial.println("⚠️ Factory Reset Requested...");
    request->send(200, "text/plain", "Factory reset...");
    delay(1000);
    preferences.begin("settings", false);
    preferences.clear();
    preferences.end();
    ESP.restart(); });

    server.begin();
    Serial.println("✅ Web Server Routes Configuration Completed!");
}
void setFrequencyInMhz(float freqMHz)
{
    Serial.println("⚙️ Setting frequency on SI5351...");

    // Power down Clock before reconfiguring
    si5351.set_clock_pwr(SI5351_CLK0, 0);
    Serial.println("🔌 Clock powered OFF");

    // Convert MHz to Hz
    uint64_t freq = (uint64_t)(freqMHz * 1e6);
    Serial.print("📡 Target frequency: ");
    Serial.print(freqMHz, 6); // show with precision
    Serial.println(" MHz");

    // Set frequency (multiply by 100 for 100ths of Hz resolution)
    si5351.set_freq(freq * 100ULL, SI5351_CLK0);
    Serial.println("📶 Frequency set successfully");

    // Power on Clock
    si5351.set_clock_pwr(SI5351_CLK0, 1);
    Serial.println("✅ SI5351 Clock powered ON");
}

void manuallyResyncTime()
{
    time_t now = time(nullptr);

    // Check if it's time to sync again
    if (now - lastManualSync < manualSyncInterval)
    {
        return; // ❌ Too soon, skip syncing
    }

    Serial.println("\n🌐 Manually triggering SNTP time sync...");

    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // Wait for sync (timeout ~5 sec)
    int retries = 0;
    while (now < 8 * 3600 * 2 && retries++ < 10)
    {
        delay(500);
        now = time(nullptr);
        Serial.print(".");
    }

    if (now > 8 * 3600 * 2)
    {
        Serial.println("\n✅ Time manually synced.");
        lastManualSync = now; // 🕒 Update last sync time
    }
    else
    {
        Serial.println("\n❌ Manual time sync failed.");
    }

    sntp_stop(); // Disable again
}

void initialTimeSynViaSNTP()
{
    // 📴 Stop any previous SNTP sync attempts
    sntp_set_sync_interval(0); // Disable automatic sync
    sntp_stop();

    // 🌐 Start SNTP with UTC offset 0 (UTC time)
    configTime(0, 0, "pool.ntp.org");

    // ⏳ Wait for SNTP time to be set
    Serial.print("⏳ Waiting for time sync");
    time_t now = time(nullptr);
    while (now < 8 * 3600 * 2)
    { // Wait until year is valid (>1970)
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }

    Serial.println("\n✅ Time synchronized!");
}
bool syncTimeFromGPS(unsigned long timeoutMs)
{
    Serial.println("📡 Trying to get time from GPS...");

    GPSserial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    unsigned long start = millis();

    while (millis() - start < timeoutMs)
    {
        while (GPSserial.available())
        {
            gps.encode(GPSserial.read());

            if (gps.time.isUpdated() && gps.date.isUpdated() &&
                gps.time.isValid() && gps.date.isValid())
            {

                struct tm timeinfo;
                timeinfo.tm_year = gps.date.year() - 1900;
                timeinfo.tm_mon = gps.date.month() - 1;
                timeinfo.tm_mday = gps.date.day();
                timeinfo.tm_hour = gps.time.hour();
                timeinfo.tm_min = gps.time.minute();
                timeinfo.tm_sec = gps.time.second();

                time_t epoch = mktime(&timeinfo);
                struct timeval now = {.tv_sec = epoch, .tv_usec = 0};
                settimeofday(&now, nullptr);

                Serial.printf("✅ GPS time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                              gps.date.year(), gps.date.month(), gps.date.day(),
                              gps.time.hour(), gps.time.minute(), gps.time.second());

                // 📍 Try to update locator from GPS location
                if (gps.location.isValid())
                {
                    float latitude = gps.location.lat();
                    float longitude = gps.location.lng();

                    String newLocator = latLonToMaidenhead(latitude, longitude);

                    if (newLocator != String(loc))
                    {
                        Serial.printf("📍 Updating stored locator: %s → %s\n", loc, newLocator.c_str());

                        preferences.begin("settings", false);
                        preferences.putString("locator", newLocator);
                        preferences.end();

                        newLocator.toCharArray(loc, sizeof(loc)); // update global variable
                    }
                    else
                    {
                        Serial.println("📍 Locator unchanged, no update needed.");
                    }
                }
                else
                {
                    Serial.println("⚠️ GPS location not valid — skipping locator update.");
                }
                gpsTimeSynced = true;
                return true; // ✅ Done
            }
        }
    }

    Serial.println("❌ GPS time sync failed. Falling back to SNTP.");
    return false;
}

String latLonToMaidenhead(float lat, float lon)
{
    char maiden[7];

    lat += 90.0;
    lon += 180.0;

    maiden[0] = 'A' + (int)(lon / 20);
    maiden[1] = 'A' + (int)(lat / 10);
    maiden[2] = '0' + (int)((lon - 20 * (int)(lon / 20)) / 2);
    maiden[3] = '0' + (int)((lat - 10 * (int)(lat / 10)) / 1);
    maiden[4] = 'a' + (int)((lon - (int)(lon / 2) * 2) * 12);
    maiden[5] = 'a' + (int)((lat - (int)(lat / 1)) * 24);
    maiden[6] = '\0';

    return String(maiden);
}


void wsprTXTask(void *parameter)
{
    tx_is_ON = true;
    tx_ON_running_time_in_s = 0;
    unsigned long lastTick = millis();

    Serial.println("🧵 TX task started on background thread.");

    // Create a timestamp counter inside the same thread (instead of separate task)
    while (tx_ON_running_time_in_s < 110 && !interruptWSPRcurrentTX)
    {
        if (millis() - lastTick >= 1000)
        {
            tx_ON_running_time_in_s++;
            lastTick = millis();
        }

        yield(); // Let other tasks run
        delay(10); // Avoid tight spin
    }

    if (!interruptWSPRcurrentTX)
    {
        transmitWSPR(); // This includes symbol-by-symbol tone sending
    }

    // Turn off clock after TX
    si5351.set_clock_pwr(SI5351_CLK0, 0);
    warmingup = false;
    tx_is_ON = false;

    Serial.println("📴 --- TX OFF: Background transmission complete ---");

    vTaskDelete(NULL); // Kill self
}
