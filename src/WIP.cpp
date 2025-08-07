#include <WiFi.h>
#include <ESPmDNS.h>
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
#define GPS_RX 16             // GPS TX → ESP32 RX2
#define GPS_TX 17             // GPS RX → ESP32 TX2
#define PPS_PIN 27            // PPS → GPIO27
#define USE_MICROS_LOOP false // Set false to go back to delay()

HardwareSerial GPSserial(2);

#define VERSION "Beta 0"
extern "C"
{
#include "esp_sntp.h"
}
// Wi-Fi credentials
const char *ssid = "MESH";
const char *password = "Nestle2010Nestle";
const char* hostname = "wspr";

// Replace with your static IP, gateway, and subnet if using static IP
IPAddress local_IP(192, 168, 0, 206);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);   // Google's DNS
IPAddress secondaryDNS(1, 1, 1, 1); // Cloudflare DNS
const int maxAttempts = 5;          // Maximum number of attempts to connect

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
unsigned long lastGPSretry = 0;
const unsigned long timeReSynchInterval = 15 * 60 * 1000; // 15 minutes
time_t lastManualSync = 0;                             // Last time we did a manual sync
const time_t manualSyncInterval = 15 * 60;             // Sync every 15 minutes
bool performCalibration = false;
bool calibrationStarted = false;
// Timing variables
volatile bool isFirstIteration = true;
volatile bool interruptWSPRcurrentTX = false;
// struct tm timeinfo;
time_t currentEpochTime;
time_t nextPosixTxTime;
time_t currentRemainingSeconds;
time_t intervalBetweenTx;

int modeOfOperation = 2;    // inherited from MLA toolbox
byte selectedBandIndex = 7; // inherited from MLA toolbox
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
// Reference duration for a full WSPR message
const unsigned long WSPR_REFERENCE_DURATION_MS = 110646;
// Async web server runs on port 80
AsyncWebServer server(80);

// prototypes

void initSI5351();
String convertPosixToHHMMSS(time_t posixTime);
void si5351_WarmingUp();
void transmitWSPR();
void startTransmission();
String formatFrequencyWithDots(unsigned freq);
void TX_ON_counter_core0(void *parameter);
void manuallyResyncTime();
void initialTimeSyncViaSNTP();
bool syncTimeFromGPS();
String latLonToMaidenhead(float lat, float lon);
void connectToWiFi();

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
void displaySelectedBandInformation(byte bandIndex);

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

    connectToWiFi();

    // Retrieve user settings
    retrieveUserSettings();
    if (!syncTimeFromGPS())
    {
        initialTimeSyncViaSNTP(); // your existing SNTP fallback
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
    if (!interruptWSPRcurrentTX)
    {
        // First-time initialization
        if (isFirstIteration)
        {
            Serial.println("\n🔁 First Loop Iteration: Determining next TX start time...");
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

        displaySelectedBandInformation(selectedBandIndex);

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

        // Break the loop if time is up or interrupted
        if (!interruptWSPRcurrentTX)
        {

            // Begin transmission
            startTransmission();
                        nextPosixTxTime += intervalBetweenTx;

        }

        // 🔁 Try to resync time from GPS if not yet synced
        unsigned long nowMs = millis();
        unsigned long elapsed = nowMs - lastGPSretry;

        long remaining = (long)timeReSynchInterval - (long)elapsed;

        if (remaining > 0)
        {
            unsigned int minutes = remaining / 60000;
            unsigned int seconds = (remaining % 60000) / 1000;
            Serial.printf("⏳ Time re-synch in %02u:%02u\n", minutes, seconds);
        }
        else
        {
            Serial.println("⏳ Time synch is due now...");
        }

        if (elapsed > timeReSynchInterval)
        {
            lastGPSretry = nowMs;
            if (syncTimeFromGPS())
            {
                Serial.println("✅ Recovered time from GPS.");
            }
            else
            {
                manuallyResyncTime();
            }
        }
    }

    if (performCalibration && !calibrationStarted)
    {
        calibrationStarted = true;
        setFrequencyInMhz(calFrequencyInMhz);
    }
    delay(5);
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
    Serial.println("🔥 Radio Module 'Warming Up' Phase Started to stabilize ...(5s before begin)");

    // 🎛️ Apply small random frequency offset (-100 to +100 Hz)

    WSPR_TX_operatingFrequ = setRandomWSPRfrequency(selectedBandIndex) * 100ULL; /// Hz * 100 for module

    // 📡 Log the new TX frequency with formatting
    Serial.print("📶 Setting TX Frequency to: ");
    Serial.print(formatFrequencyWithDots(WSPR_TX_operatingFrequ / 100ULL));
    Serial.print(" (ref. ");
    Serial.print(formatFrequencyWithDots(TX_referenceFrequ));
    Serial.println(")");
    // ⚙️ Configure Si5351 for transmission
    si5351.set_freq(WSPR_TX_operatingFrequ, SI5351_CLK0);
    si5351.set_clock_pwr(SI5351_CLK0, 1); // Power ON
}

void startTransmission()
{
    tx_is_ON = true;

    xTaskCreatePinnedToCore(
        TX_ON_counter_core0,  // Task function
        "TXCounterTask",      // Name of the task
        1536,                 // Stack size in words
        NULL,                 // Task input parameter
        1,                    // Priority of the task
        &txCounterTaskHandle, // Task handle
        0                     // Core to pin the task to (0 in this case)
    );

    transmitWSPR();
    tx_is_ON = false;
    tx_ON_running_time_in_s = 0;

    // Wait for the task to complete and clean up
    vTaskDelete(txCounterTaskHandle);
    txCounterTaskHandle = NULL;
}

// ✅ Returns a randomized safe WSPR transmit frequency for a given band index
unsigned long setRandomWSPRfrequency(byte bandIndex)
{
    Serial.println("🎯 Selecting random freq within available range");

    unsigned long officialStart = WSPRbandStart[bandIndex];
    unsigned long officialEnd = WSPRbandEnd[bandIndex];
    unsigned long minF = officialStart + 3;
    unsigned long maxF = officialEnd - 3;

    unsigned long freq = random(minF, maxF + 1); // inclusive range

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

void TX_ON_counter_core0(void *parameter)
{
    tx_ON_running_time_in_s = 0;

    while (tx_is_ON == true)
    {
        tx_ON_running_time_in_s = tx_ON_running_time_in_s + 1;
        // The transmission occupies exactly 110.592 seconds ( rounded to 110).

        // Serial.printf("TX ON counter: %d seconds\n", tx_ON_running_time_in_s);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // Delete the task if tx_is_ON is false to free resources
    vTaskDelete(NULL);
    txCounterTaskHandle = NULL;
}

void displaySelectedBandInformation(byte bandIndex)
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
    Serial.println("📡 --- TX ON: Transmission Started ---");
    // Record start time (both POSIX and millis)
    unsigned long txStartEpoch = time(nullptr);
    unsigned long txStartMillis = millis();

    // 🔊 Transmit each WSPR symbol

    for (int i = 0; i < SYMBOL_COUNT; i++)
    {
        uint64_t toneFreq = WSPR_TX_operatingFrequ + (tx_buffer[i] * TONE_SPACING);
        si5351.set_freq(toneFreq, SI5351_CLK0);

        delay(WSPR_DELAY - 1); // 1 from experiments
        yield();

        Serial.print(">");
        if (interruptWSPRcurrentTX)
            break;
    }
    // Record end time
    unsigned long txEndEpoch = time(nullptr);
    unsigned long txEndMillis = millis();
    // Shutdown Si5351 output after TX
    si5351.set_clock_pwr(SI5351_CLK0, 0);
    Serial.println("\n📴 --- TX OFF: Transmission Complete ---\n");

    // --- Calculate durations ---
    unsigned long txDuration = txEndMillis - txStartMillis;
    unsigned int minutes = txDuration / 60000;
    unsigned int seconds = (txDuration % 60000) / 1000;
    unsigned int milliseconds = txDuration % 1000;

    Serial.printf("⏱️ TX Duration: %lu ms (%02d:%02d & %03d ms)\n",
                  txDuration, minutes, seconds, milliseconds);

    // --- Delta with reference ---
    long delta = (long)txDuration - (long)WSPR_REFERENCE_DURATION_MS;
    Serial.printf("📏 Delta vs Reference (110646 ms): %+ld ms\n", delta);

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
    request->send(FILESYSTEM, "/index.html", "text/html"); });

    server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    request->send(FILESYSTEM, "/index.html", "text/html"); });

    server.on("/calibrate.html", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    request->send(FILESYSTEM, "/calibrate.html", "text/html"); });

    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    request->send(FILESYSTEM, "/favicon.ico", "image/x-icon"); });

    // Static assets
    server.on("/assets/spectrum.jpg", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    request->send(FILESYSTEM, "/assets/spectrum.jpg", "image/jpeg"); });

    server.on("/assets/wsprlogo.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
    request->send(FILESYSTEM, "/assets/wsprlogo.png", "image/png"); });
    server.on("/assets/warning.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
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
    Serial.println("🛠️ Entering Calibration Mode");
    interruptWSPRcurrentTX = true;
    performCalibration=true;
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
      Serial.println("✅ Exiting Calibration Mode, Rebooting");
      ESP.restart();
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

void initialTimeSynViaSNTPXXXX()
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

void initialTimeSyncViaSNTP()
{
    const char *ntpServers[] = {
        "pool.ntp.org",
        "time.nist.gov",
        "time.google.com",
        "europe.pool.ntp.org",
        "ntp1.inrim.it", // Italy 🇮🇹
    };

    const int maxAttemptsPerServer = 3;
    const int numServers = sizeof(ntpServers) / sizeof(ntpServers[0]);

    sntp_set_sync_interval(0); // Disable auto-sync
    sntp_stop();

    for (int i = 0; i < numServers; i++)
    {
        Serial.printf("🌐 Trying SNTP server: %s\n", ntpServers[i]);

        configTime(0, 0, ntpServers[i]);

        for (int attempt = 0; attempt < maxAttemptsPerServer; attempt++)
        {
            Serial.print("⏳ Waiting for time sync");
            time_t now = time(nullptr);
            int wait_ms = 0;

            while (now < 8 * 3600 * 2 && wait_ms < 5000)
            { // wait max 5s
                delay(250);
                Serial.print(".");
                wait_ms += 250;
                now = time(nullptr);
            }

            if (now >= 8 * 3600 * 2)
            {
                Serial.println("\n✅ Time synchronized!");
                return;
            }

            Serial.println(" ⚠️ Attempt failed.");
        }
    }

    Serial.println("❌ All SNTP servers failed.");
}

bool syncTimeFromGPS()
{
    Serial.println("📡 Trying to get time from GPS...");

    GPSserial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX); // Ensure correct serial initialization
    unsigned long start = millis();

    while (millis() - start < 10000) // Increased timeout to 10 seconds
    {
        while (GPSserial.available())
        {
            gps.encode(GPSserial.read()); // Read all available GPS data
        }

        if (gps.time.isUpdated() && gps.date.isUpdated() && gps.time.isValid() && gps.date.isValid())
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

            return true; // ✅ Done
        }

        // Debug message to indicate GPS is still trying
        // Serial.println("Debug: GPS time not updated yet, waiting...");
        delay(100); // Small delay to prevent overwhelming the loop
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

void connectToWiFi()
{
    int attempts = 0;
    bool connected = false;

    // Retry loop to connect to Wi-Fi
    WiFi.setHostname(hostname); // Apply DHCP hostname
        WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);

    while (attempts < maxAttempts && !connected)
    {
        WiFi.begin(ssid, password);

        Serial.print("Connecting to Wi-Fi");
        while (WiFi.status() != WL_CONNECTED)
        {
            delay(500);
            Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            connected = true;
            Serial.println("\nConnected to Wi-Fi");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
        }
        else
        {
            Serial.println("\nFailed to connect to Wi-Fi, retrying...");
            attempts++;
            delay(1000); // Wait before retrying
        }
    }
    // ✅ Start mDNS after Wi-Fi is connected
    if (MDNS.begin(hostname)) {
        Serial.println("✅ mDNS responder started: http://wspr.local");
    } else {
        Serial.println("❌ Error starting mDNS");
    }
    // If not connected after maxAttempts, reset the ESP32
    if (!connected)
    {
        Serial.println("Failed to connect after multiple attempts. Resetting ESP32...");
        ESP.restart();
    }
}

void connectToWiFiNEW()
{
    int attempts = 0;
    bool connected = false;
    IPAddress dhcpIP;

    // Retry loop to connect to Wi-Fi via DHCP
    while (attempts < maxAttempts && !connected)
    {
        WiFi.begin(ssid, password);

        Serial.print("Connecting to Wi-Fi (DHCP)...");
        while (WiFi.status() != WL_CONNECTED)
        {
            delay(500);
            Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            connected = true;
            Serial.println("\nConnected to Wi-Fi via DHCP");
            dhcpIP = WiFi.localIP(); // Get the IP assigned by DHCP
            Serial.print("IP Address: ");
            Serial.println(dhcpIP);

            // Retrieve and print gateway and subnet
            gateway = WiFi.gatewayIP();
            subnet = WiFi.subnetMask();

            Serial.print("Gateway IP: ");
            Serial.println(gateway);

            Serial.print("Subnet Mask: ");
            Serial.println(subnet);
        }
        else
        {
            Serial.println("\nFailed to connect to Wi-Fi, retrying...");
            attempts++;
            delay(1000); // Wait before retrying
        }
    }

    // If not connected after maxAttempts, reset the ESP32
    if (!connected)
    {
        Serial.println("Failed to connect after multiple attempts. Resetting ESP32...");
        ESP.restart();
    }

    // Disconnect from Wi-Fi
    WiFi.disconnect();
    delay(500); // Wait for disconnect to complete

    // Reconnect with the IP assigned by DHCP (using found gateway and subnet)
    Serial.println("Reconnecting with DHCP IP...");
    WiFi.config(dhcpIP, gateway, subnet); // Use the DHCP IP as static IP
    WiFi.begin(ssid, password);

    // Wait until connected with the DHCP-assigned IP
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nReconnected to Wi-Fi with DHCP-assigned IP");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}