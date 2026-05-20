#ifndef GSM_H
#define GSM_H

#include <Arduino.h>

// Initialize the SIM800L module using the provided pins
bool gsm_init(uint8_t rx_pin, uint8_t tx_pin);

bool gsm_is_connected();
bool gsm_send_mqtt_payload(const char* topic, const char* payload);

#endif