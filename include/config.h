#pragma once
// =============================================================================
// C-TRANSIT TERMINAL — PRODUCTION CONFIGURATION
// Single Source of Truth for all Hardware and Cloud constants.
// =============================================================================

// ── 1. SYSTEM IDENTITY ────────────────────────────────────────────────────────
#define TERMINAL_ID "TERM_01"
#define FIRMWARE_VERSION "v2.0-PROD"

// ── 2. NETWORK & CLOUD (Secure MQTT) ──────────────────────────────────────────
#define WIFI_SSID "0x1324646"             // <-- Update to your local Wi-Fi
#define WIFI_PASS "tensazangetsu12"         // <-- Update to your local Wi-Fi
#define WIFI_CONNECT_TIMEOUT_MS 15000UL

#define MQTT_HOST          "c698857529f142c98dd9bf344260ab0a.s1.eu.hivemq.cloud"
#define MQTT_PORT          8883
#define MQTT_USERNAME      "c-transit"
#define MQTT_PASSWORD      "B4c-Transitcuit4cu@2"
#define MQTT_CLIENT_ID     TERMINAL_ID
#define MQTT_KEEPALIVE_S   60
#define MQTT_QOS           1

// Cloud Topics (Strict matching with Node.js backend)
#define MQTT_TOPIC_TX      "ctransit/" TERMINAL_ID "/tx"
#define MQTT_TOPIC_RX      "ctransit/" TERMINAL_ID "/rx"
#define MQTT_TOPIC_STATUS  "ctransit/" TERMINAL_ID "/status"

#define MQTT_LWT_OFFLINE   "OFFLINE"
#define MQTT_LWT_ONLINE    "ONLINE"
#define MQTT_PAYLOAD_BUF   4096

// ── 3. HARDWARE PINOUTS ───────────────────────────────────────────────────────
// UI (LEDs & Buzzer)
#define PIN_LED_GREEN 2
#define PIN_LED_RED   4
#define PIN_BUZZER    13

// RFID Scanner (SPI)
#define PIN_RFID_SS   5
#define PIN_RFID_RST  27
#define PIN_RFID_SCK  18
#define PIN_RFID_MISO 19
#define PIN_RFID_MOSI 23

// 4x4 Keypad
static const uint8_t KEYPAD_ROW_PINS[4] = {32, 33, 15, 12};
static const uint8_t KEYPAD_COL_PINS[4] = {0, 14, 26, 25};
static const char    KEYPAD_MAP[4][4]   = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

// LCD Display
#define LCD_I2C_ADDR 0x27
#define LCD_COLS     16
#define LCD_ROWS     2

// ── 4. LITTLEFS FILE PATHS ────────────────────────────────────────────────────
#define FILE_WHITELIST "/whitelist.db"
#define FILE_BLACKLIST "/blacklist.db"
#define FILE_DRIVERS   "/drivers.db"
#define FILE_ADMINS    "/admins.db"
#define FILE_TX_LOG    "/tx_log.db"

// ── 5. TIMING & SYSTEM BEHAVIOR ───────────────────────────────────────────────
#define SYNC_INTERVAL_MS        5000UL    // How often to flush unsent logs
#define SYNC_TIMEOUT_SECONDS    10800UL   // Force reboot if sync fails for 3 hrs
#define TX_LOG_MAX_LINES        2000      // Prevent memory overflow
#define MAX_OFFLINE_TAPS_PER_UID 2
#define FARE_AMOUNT             -200

#define BEEP_SHORT_MS   150
#define BEEP_LONG_MS    800
#define LED_FEEDBACK_MS 2000
#define LCD_RESULT_MS   2000

// ── 6. FREERTOS TASK ALLOCATION ───────────────────────────────────────────────
#define CORE_APP  0  // UI, RFID, Keypad run here
#define CORE_SYNC 1  // Wi-Fi and MQTT run here
#define PRIORITY_SYNC 1
#define PRIORITY_RFID 2
#define STACK_SIZE_SYNC 8192
#define STACK_SIZE_APP  8192