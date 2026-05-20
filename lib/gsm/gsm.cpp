#include "gsm.h"
#include "logger.h" // Assuming you want to use your existing logging system


// Use Hardware Serial 2 for the GSM module
HardwareSerial sim800(2); 

// =============================================================================
// gsm_init
// Wakes up the module and checks if it's alive
// =============================================================================
bool gsm_init(uint8_t rx_pin, uint8_t tx_pin) {
    LOG_INFO("GSM", "Initializing SIM800L on RX:%d TX:%d...", rx_pin, tx_pin);
    
    // Start the serial connection using the pins passed from main.cpp
    sim800.begin(9600, SERIAL_8N1, rx_pin, tx_pin);
    
    // Give it a second to wake up
    vTaskDelay(pdMS_TO_TICKS(1000)); 

    // Send a basic AT ping
    sim800.println("AT");
    vTaskDelay(pdMS_TO_TICKS(500));
    
    if (sim800.available()) {
        String response = sim800.readString();
        if (response.indexOf("OK") != -1) {
            LOG_INFO("GSM", "SIM800L is ALIVE and responding!");
            return true;
        }
    }
    
    LOG_ERROR("GSM", "Failed to communicate with SIM800L.");
    return false;
}

// =============================================================================
// gsm_is_connected
// Checks network registration
// =============================================================================
bool gsm_is_connected() {
    sim800.println("AT+CREG?");
    vTaskDelay(pdMS_TO_TICKS(500));
    
    if (sim800.available()) {
        String response = sim800.readString();
        // 0,1 means registered home network. 0,5 means roaming.
        if (response.indexOf("0,1") != -1 || response.indexOf("0,5") != -1) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// gsm_send_mqtt_payload (Placeholder for now)
// =============================================================================
bool gsm_send_mqtt_payload(const char* topic, const char* payload) {
    // We will write the complex AT commands for GPRS/MQTT later!
    LOG_INFO("GSM", "Simulating sending payload: %s", payload);
    return true; 
}