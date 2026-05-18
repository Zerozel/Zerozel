// =============================================================================
// sync.cpp  —  Core 1 Always-On MQTT sync via WiFiClientSecure (port 8883)
// =============================================================================
#include "sync.h"
#include "../storage/storage.h"
#include "../transaction/transaction.h"
#include "../logger/logger.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// <-- ADDED: Fallback if not in config.h
#ifndef PUBACK_TIMEOUT_MS
#define PUBACK_TIMEOUT_MS 3000
#endif

// ── Animation timestamps (volatile — read by Core 0 UI task) ─────────────────
volatile uint32_t g_last_upload_ms   = 0;
volatile uint32_t g_last_download_ms = 0;

// ── Module state ─────────────────────────────────────────────────────────────
static WiFiClientSecure s_tls;
static PubSubClient     s_mqtt(s_tls);
static TaskHandle_t     s_task_handle  = nullptr;
static volatile bool    s_running      = false;
static bool             s_cold_pending = false;
static bool             s_sync_done    = false;

// ── Downlink parse buffer (fixed-size — no heap String on Core 1) ─────────────
static char s_dl_buf[512];

// ── MQTT callback ─────────────────────────────────────────────────────────────
static void _mqtt_cb(char* topic, byte* payload, unsigned int len){
    size_t cp=min((size_t)len,sizeof(s_dl_buf)-1);
    memcpy(s_dl_buf,payload,cp); s_dl_buf[cp]='\0';
    g_last_download_ms = millis();   // ← animation timestamp
    LOG_INFO("SYNC","RX [%s]: %s",topic,s_dl_buf);
    sync_process_downlink(s_dl_buf,(unsigned int)cp);
}

// ── process_downlink ──────────────────────────────────────────────────────────
void sync_process_downlink(const char* pay, unsigned int len){
    if(!pay||!len) return;

    // SYS:SYNC_COMPLETE
    if(strncmp(pay,"SYS:SYNC_COMPLETE",17)==0){ s_sync_done=true; return; }

    // SYS:WL,uid1|uid2  or  SYS:BL,uid1|uid2
    if(strncmp(pay,"SYS:",4)==0){
        char tmp[512]; strncpy(tmp,pay,sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
        char* colon=strchr(tmp+4,':');
        if(!colon) return;
        char list[3]={0}; strncpy(list,tmp+4,2);
        const char* uids=colon+1;
        const char* fp=nullptr;
        if(strcmp(list,"WL")==0)      fp=FILE_WHITELIST;
        else if(strcmp(list,"BL")==0) fp=FILE_BLACKLIST;
        else if(strcmp(list,"DR")==0) fp=FILE_DRIVERS;
        else if(strcmp(list,"AD")==0) fp=FILE_ADMINS;
        if(fp) storage_ingest_chunk(fp,uids);
        return;
    }

    // ADD:BL,uid|REM:WL,uid  differential
    char buf[512]; strncpy(buf,pay,sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
    char* cmd=strtok(buf,"|");
    while(cmd){
        char act[4]={0}, lst[3]={0}, uid[9]={0};
        if(sscanf(cmd,"%3[^:]:%2[^,],%8s",act,lst,uid)<3){ cmd=strtok(nullptr,"|"); continue; }
        const char* fp=nullptr;
        if(strcmp(lst,"WL")==0)      fp=FILE_WHITELIST;
        else if(strcmp(lst,"BL")==0) fp=FILE_BLACKLIST;
        else if(strcmp(lst,"DR")==0) fp=FILE_DRIVERS;
        else if(strcmp(lst,"AD")==0) fp=FILE_ADMINS;
        if(fp){
            if(strcmp(act,"ADD")==0)      storage_append_uid(fp,uid);
            else if(strcmp(act,"REM")==0) storage_remove_uid(fp,uid);
        }
        cmd=strtok(nullptr,"|");
    }
}

// ── WiFi connect ──────────────────────────────────────────────────────────────
static bool _wifi_connect(){
    if(WiFi.status()==WL_CONNECTED) return true;
    LOG_INFO("SYNC","Connecting to WiFi '%s'...",WIFI_SSID);
    WiFi.begin(WIFI_SSID,WIFI_PASS);
    uint32_t t=millis();
    while(WiFi.status()!=WL_CONNECTED && (millis()-t)<WIFI_CONNECT_TIMEOUT_MS)
        vTaskDelay(pdMS_TO_TICKS(500));
    if(WiFi.status()!=WL_CONNECTED){ LOG_ERROR("SYNC","WiFi failed"); return false; }
    LOG_INFO("SYNC","WiFi OK  IP=%s",WiFi.localIP().toString().c_str());
    return true;
}

// ── MQTT connect with LWT ─────────────────────────────────────────────────────
static bool _mqtt_connect(){
    if(s_mqtt.connected()) return true;

    // TLS — skip cert verification for initial deployment
    // For production: load ISRG Root X1 PEM via s_tls.setCACert(ca_pem)
    s_tls.setInsecure();

    s_mqtt.setServer(MQTT_HOST,MQTT_PORT);
    s_mqtt.setCallback(_mqtt_cb);
    s_mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
    s_mqtt.setBufferSize(MQTT_PAYLOAD_BUF);

    LOG_INFO("SYNC","MQTT connect → %s:%d",MQTT_HOST,MQTT_PORT);

    // connect() with LWT pre-registered (SAD requirement)
    bool ok=s_mqtt.connect(
        MQTT_CLIENT_ID,
        MQTT_USERNAME, MQTT_PASSWORD, // <-- ADDED: Actually pass the username/password to connect!
        MQTT_TOPIC_STATUS, MQTT_QOS, true, MQTT_LWT_OFFLINE
    );
    if(!ok){ LOG_ERROR("SYNC","MQTT connect failed state=%d",s_mqtt.state()); return false; }

    // Immediately publish ONLINE to override the LWT
    s_mqtt.publish(MQTT_TOPIC_STATUS, MQTT_LWT_ONLINE, true);
    LOG_INFO("SYNC","MQTT connected, published ONLINE");

    s_mqtt.subscribe(MQTT_TOPIC_RX, MQTT_QOS);
    LOG_INFO("SYNC","Subscribed to %s",MQTT_TOPIC_RX);
    return true;
}

// ── Flush tx.log ──────────────────────────────────────────────────────────────
static void _flush_tx(){
    static char pay[MQTT_PAYLOAD_BUF];
    size_t bytes_read=0;
    int lines=storage_stream_tx_chunk(pay,sizeof(pay),&bytes_read);
    if(lines==0){ LOG_INFO("SYNC","tx.log empty"); return; }

    LOG_INFO("SYNC","Publishing %d lines (%zu bytes)",lines,bytes_read);
    bool ok=s_mqtt.publish(MQTT_TOPIC_TX,(uint8_t*)pay,(unsigned int)strlen(pay),false);

    if(!ok){ LOG_ERROR("SYNC","Publish failed — tx.log preserved"); return; }

    // Wait for PUBACK (PubSubClient QoS1 handling)
    uint32_t t=millis();
    while((millis()-t)<PUBACK_TIMEOUT_MS) s_mqtt.loop(), vTaskDelay(pdMS_TO_TICKS(50));

    g_last_upload_ms = millis();   // ← animation timestamp

    // Atomic delete after confirmed publish
    storage_atomic_delete_sent(bytes_read);

    // Update 3-hour kill-switch clock
    unsigned long now=transaction_get_ts();
    storage_write_sync_ts(now);
    LOG_INFO("SYNC","Flush complete, sync.dat=%lu",now);
}

// ── Cold start full sync ──────────────────────────────────────────────────────
static void _cold_start_sync(){
    LOG_INFO("SYNC","Requesting full DB sync...");
    s_mqtt.publish(MQTT_TOPIC_TX,"SYS:REQ_FULL_SYNC");
    s_sync_done=false;
    uint32_t t=millis();
    while(!s_sync_done&&(millis()-t)<120000){ s_mqtt.loop(); vTaskDelay(pdMS_TO_TICKS(100)); }
    if(s_sync_done){ LOG_INFO("SYNC","Cold sync complete"); s_cold_pending=false; }
    else LOG_ERROR("SYNC","Cold sync timeout");
}

// ── Public API ────────────────────────────────────────────────────────────────
void sync_init(){
    s_cold_pending=(storage_get_file_size(FILE_WHITELIST)==0||
                    storage_get_file_size(FILE_DRIVERS)==0);
    LOG_INFO("SYNC","init cold=%s",s_cold_pending?"YES":"NO");
}

void sync_set_task_handle(TaskHandle_t h){ s_task_handle=h; }
bool sync_is_running()                   { return s_running; }

void sync_trigger_now(){
    if(s_task_handle) xTaskNotify(s_task_handle,1,eSetValueWithOverwrite);
}

// ── Core 1 task ───────────────────────────────────────────────────────────────
void sync_task(void* params){
    (void)params;
    LOG_INFO("SYNC","Core 1 task start");
    vTaskDelay(pdMS_TO_TICKS(3000));  // let Core 0 finish hardware init
    sync_init();

    while(true){
        // Block until timer fires or sync_trigger_now() notifies
        uint32_t val=0;
        xTaskNotifyWait(0,ULONG_MAX,&val,pdMS_TO_TICKS(SYNC_INTERVAL_MS));

        s_running=true;

        // Step 1: WiFi
        if(!_wifi_connect()){ s_running=false; continue; }

        // Step 2: MQTT
        if(!_mqtt_connect()){ s_running=false; continue; }

        // Step 3: Cold start if needed
        if(s_cold_pending) _cold_start_sync();

        // Step 4: Flush tx.log
        _flush_tx();

        // Step 5: Drain pending downlink messages
        uint32_t drain=millis();
        while((millis()-drain)<3000){ s_mqtt.loop(); vTaskDelay(pdMS_TO_TICKS(100)); }

        s_running=false;
    }
}