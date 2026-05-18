// =============================================================================
// storage.cpp  —  LittleFS implementation
// =============================================================================
#include "storage.h"
#include "../logger/logger.h" 
#include <LittleFS.h>
#include <freertos/semphr.h>

#ifndef FILE_SESSION
#define FILE_SESSION "/session.db"
#endif
#ifndef FILE_SYNC_TS
#define FILE_SYNC_TS "/sync_ts.db"
#endif
#ifndef FILE_TX_TEMP
#define FILE_TX_TEMP "/tx_tmp.db"
#endif
#ifndef TX_LINE_MAX
#define TX_LINE_MAX 80
#endif

// Minimum free bytes that must remain on the filesystem before any write.
static const size_t FS_HEADROOM_BYTES = 8192;

static SemaphoreHandle_t s_mtx = nullptr;

static bool _lock()  { return s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(3000))==pdTRUE; }
static void _unlock(){ if(s_mtx) xSemaphoreGive(s_mtx); }

static bool _has_space(size_t needed = 256){
    size_t free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if(free_bytes < (needed + FS_HEADROOM_BYTES)){
        LOG_ERROR("STORAGE","Insufficient space: free=%zu need=%zu headroom=%zu",
                  free_bytes, needed, FS_HEADROOM_BYTES);
        return false;
    }
    return true;
}

// =============================================================================
// storage_init
// =============================================================================
bool storage_init(){
    s_mtx = xSemaphoreCreateMutex();
    if(!s_mtx){ LOG_ERROR("STORAGE","Mutex create failed"); return false; }

    if(!LittleFS.begin(true, "/littlefs", 10, "spiffs")){
        LOG_ERROR("STORAGE", "LittleFS Mount Failed. Ensure partitions.csv uses 'spiffs' label.");
        return false;
    }

    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    LOG_INFO("STORAGE","LittleFS mounted  total=%zuKB  used=%zuKB  free=%zuKB",
             total/1024, used/1024, (total-used)/1024);

    const char* needed[] = {
        FILE_WHITELIST, FILE_BLACKLIST, FILE_TX_LOG,
        FILE_SYNC_TS,   FILE_DRIVERS,   FILE_ADMINS
    };
    for(auto p : needed){
        if(!LittleFS.exists(p)){
            File f = LittleFS.open(p, "w");
            if(f){ f.close(); LOG_WARN("STORAGE","Created %s", p); }
            else { LOG_ERROR("STORAGE","Could not create %s", p); }
        }
    }
    if(!LittleFS.exists(FILE_SESSION)) storage_write_session(0, "NONE");
    return true;
}

StorageResult storage_uid_in_file(const char* path, const char* uid){
    if(!_lock()) return STORAGE_ERROR;
    File f = LittleFS.open(path, "r");
    if(!f){ _unlock(); return STORAGE_NOT_FOUND; }
    StorageResult res = STORAGE_NOT_FOUND;
    while(f.available()){
        String line = f.readStringUntil('\n');
        line.trim();
        if(!line.length()) continue;
        int c = line.indexOf(',');
        String fu = (c >= 0) ? line.substring(0, c) : line;
        if(fu.equals(uid)){ res = STORAGE_FOUND; break; }
    }
    f.close(); _unlock();
    return res;
}

StorageResult storage_append_uid(const char* path, const char* uid){
    if(!_lock()) return STORAGE_ERROR;
    if(!_has_space(32)){ _unlock(); return STORAGE_FULL; }
    File f = LittleFS.open(path, "a");
    if(!f){ _unlock(); return STORAGE_ERROR; }
    f.printf("%s\n", uid);
    f.close(); _unlock();
    LOG_DEBUG("STORAGE","append_uid %s → %s", uid, path);
    return STORAGE_OK;
}

StorageResult storage_remove_uid(const char* path, const char* uid){
    if(!_lock()) return STORAGE_ERROR;
    if(!_has_space(128)){ _unlock(); return STORAGE_FULL; }

    File src = LittleFS.open(path,        "r");
    File dst = LittleFS.open(FILE_TX_TEMP,"w");
    if(!src || !dst){
        if(src) src.close();
        if(dst) dst.close();
        _unlock(); return STORAGE_ERROR;
    }

    bool found = false;
    while(src.available()){
        String line = src.readStringUntil('\n');
        line.trim();
        if(!line.length()) continue;
        int c = line.indexOf(',');
        String fu = (c >= 0) ? line.substring(0, c) : line;
        if(fu.equals(uid)){ found = true; continue; }
        dst.println(line);
    }
    src.close(); dst.close();

    LittleFS.remove(path);
    LittleFS.rename(FILE_TX_TEMP, path);
    _unlock();
    return found ? STORAGE_OK : STORAGE_NOT_FOUND;
}

StorageResult storage_append_tx(const char* uid, int amt, unsigned long ts, const char* drv){
    if(!_lock()) return STORAGE_ERROR;

    int cnt = 0;
    {
        File c = LittleFS.open(FILE_TX_LOG, "r");
        if(c){
            while(c.available()){
                String l = c.readStringUntil('\n');
                l.trim(); // <-- FIXED: Split into two lines
                if(l.length()) cnt++;
            }
            c.close();
        }
    }

    if(cnt >= TX_LOG_MAX_LINES){
        _unlock();
        LOG_WARN("STORAGE","tx.log at hard cap (%d lines)", TX_LOG_MAX_LINES);
        return STORAGE_FULL;
    }

    if(!_has_space(128)){ _unlock(); return STORAGE_FULL; }

    File f = LittleFS.open(FILE_TX_LOG, "a"); 
    if(!f){ _unlock(); return STORAGE_ERROR; }

    f.printf("%s,%d,%lu,%s\n", uid, amt, ts, drv);
    f.close(); _unlock();

    LOG_DEBUG("STORAGE","tx appended line %d: %s,%d,%lu", cnt+1, uid, amt, ts);
    return STORAGE_OK;
}

int storage_count_uid_in_tx(const char* uid){
    if(!_lock()) return -1;
    File f = LittleFS.open(FILE_TX_LOG, "r");
    if(!f){ _unlock(); return 0; }
    int cnt = 0;
    while(f.available()){
        String l = f.readStringUntil('\n');
        l.trim();
        if(!l.length()) continue;
        int c = l.indexOf(',');
        if(c < 0) continue;
        if(l.substring(0, c).equals(uid)) cnt++;
    }
    f.close(); _unlock();
    return cnt;
}

int storage_get_tx_line_count(){
    if(!_lock()) return -1;
    File f = LittleFS.open(FILE_TX_LOG, "r");
    if(!f){ _unlock(); return 0; }
    int cnt = 0;
    while(f.available()){
        String l = f.readStringUntil('\n');
        l.trim(); // <-- FIXED: Split into two lines
        if(l.length()) cnt++;
    }
    f.close(); _unlock();
    return cnt;
}

StorageResult storage_get_pin_for_uid(const char* path, const char* uid, char* out, size_t sz){
    if(!out || sz < 2) return STORAGE_ERROR;
    if(!_lock()) return STORAGE_ERROR;
    File f = LittleFS.open(path, "r");
    if(!f){ _unlock(); return STORAGE_NOT_FOUND; }
    StorageResult res = STORAGE_NOT_FOUND;
    while(f.available()){
        String line = f.readStringUntil('\n');
        line.trim();
        if(!line.length()) continue;
        int c = line.indexOf(',');
        if(c < 0) continue;
        if(line.substring(0, c).equals(uid)){
            strncpy(out, line.substring(c+1).c_str(), sz-1);
            out[sz-1] = '\0';
            res = STORAGE_FOUND;
            break;
        }
    }
    f.close(); _unlock();
    return res;
}

StorageResult storage_read_session(SessionData* out){
    if(!out) return STORAGE_ERROR;
    out->active = 0;
    strncpy(out->driver_uid, "NONE", sizeof(out->driver_uid));
    if(!_lock()) return STORAGE_ERROR;
    File f = LittleFS.open(FILE_SESSION, "r");
    if(!f){ _unlock(); return STORAGE_NOT_FOUND; }
    String line = f.readStringUntil('\n');
    f.close(); _unlock();
    line.trim();
    int c = line.indexOf(',');
    if(c < 0) return STORAGE_ERROR;
    out->active = (uint8_t)line.substring(0, c).toInt();
    strncpy(out->driver_uid, line.substring(c+1).c_str(), sizeof(out->driver_uid)-1);
    out->driver_uid[sizeof(out->driver_uid)-1] = '\0';
    return STORAGE_OK;
}

StorageResult storage_write_session(uint8_t active, const char* uid){
    if(!_lock()) return STORAGE_ERROR;
    if(!_has_space(32)){ _unlock(); return STORAGE_FULL; }
    File f = LittleFS.open(FILE_SESSION, "w");
    if(!f){ _unlock(); return STORAGE_ERROR; }
    f.printf("%d,%s\n", active, uid ? uid : "NONE");
    f.close(); _unlock();
    return STORAGE_OK;
}

unsigned long storage_read_sync_ts(){
    if(!_lock()) return 0;
    File f = LittleFS.open(FILE_SYNC_TS, "r");
    if(!f){ _unlock(); return 0; }
    String l = f.readStringUntil('\n');
    f.close(); _unlock();
    l.trim(); // <-- FIXED: Split into two lines
    return (unsigned long)l.toInt();
}

StorageResult storage_write_sync_ts(unsigned long ts){
    if(!_lock()) return STORAGE_ERROR;
    if(!_has_space(32)){ _unlock(); return STORAGE_FULL; }
    File f = LittleFS.open(FILE_SYNC_TS, "w");
    if(!f){ _unlock(); return STORAGE_ERROR; }
    f.printf("%lu\n", ts);
    f.close(); _unlock();
    LOG_DEBUG("STORAGE","sync.dat = %lu", ts);
    return STORAGE_OK;
}

size_t storage_get_file_size(const char* path){
    if(!LittleFS.exists(path)) return 0;
    File f = LittleFS.open(path, "r");
    if(!f) return 0;
    size_t sz = f.size();
    f.close();
    return sz;
}

int storage_stream_tx_chunk(char* buf, size_t bufsz, size_t* bytes_read){
    if(!buf || !bytes_read) return 0;
    *bytes_read = 0;
    if(!_lock()) return 0;

    File f = LittleFS.open(FILE_TX_LOG, "r");
    if(!f){ _unlock(); return 0; }

    int    lines = 0;
    size_t used  = 0;

    while(f.available()){
        String line = f.readStringUntil('\n');
        line.trim();
        if(!line.length()) continue;

        char piece[TX_LINE_MAX + 32];
        int plen = (lines == 0)
            ? snprintf(piece, sizeof(piece), "%s:%s",  TERMINAL_ID, line.c_str())
            : snprintf(piece, sizeof(piece), "|%s",                 line.c_str());

        if(used + (size_t)plen + 1 >= bufsz) break;
        memcpy(buf + used, piece, plen);
        used += plen;
        lines++;
        *bytes_read = f.position(); 
    }

    f.close(); _unlock();
    buf[used] = '\0';
    LOG_INFO("STORAGE","tx chunk: %d lines, %zu bytes", lines, used);
    return lines;
}

StorageResult storage_atomic_delete_sent(size_t bytes_sent){
    if(bytes_sent == 0) return STORAGE_OK;
    if(!_lock()) return STORAGE_ERROR;
    if(!_has_space(256)){ _unlock(); return STORAGE_FULL; }

    File src = LittleFS.open(FILE_TX_LOG,  "r");
    File dst = LittleFS.open(FILE_TX_TEMP, "w");
    if(!src || !dst){
        if(src) src.close();
        if(dst) dst.close();
        _unlock(); return STORAGE_ERROR;
    }

    src.seek(bytes_sent); 
    while(src.available()) dst.write(src.read());
    src.close(); dst.close();

    LittleFS.remove(FILE_TX_LOG);
    LittleFS.rename(FILE_TX_TEMP, FILE_TX_LOG);

    _unlock();
    LOG_INFO("STORAGE","atomic_delete: removed %zu sent bytes", bytes_sent);
    return STORAGE_OK;
}

StorageResult storage_ingest_chunk(const char* path, const char* uid_list){
    if(!path || !uid_list) return STORAGE_ERROR;
    if(!_lock()) return STORAGE_ERROR;
    if(!_has_space(512)){ _unlock(); return STORAGE_FULL; }

    File f = LittleFS.open(path, "a"); 
    if(!f){ _unlock(); return STORAGE_ERROR; }

    char buf[512];
    strncpy(buf, uid_list, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    int cnt = 0;
    char* tok = strtok(buf, "|");
    while(tok){
        while(*tok == ' ') tok++;
        if(strlen(tok)){ f.printf("%s\n", tok); cnt++; }
        tok = strtok(nullptr, "|");
    }

    f.close(); _unlock();
    LOG_INFO("STORAGE","ingest_chunk: %d UIDs → %s", cnt, path);
    return STORAGE_OK;
}