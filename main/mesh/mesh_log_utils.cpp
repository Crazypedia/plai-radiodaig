/**
 * @file mesh_log_utils.cpp
 * @brief Utilities for parsing and loading Meshtastic NDJSON logs.
 */
#include "mesh_log_utils.h"
#include "mesh_logger.h"
#include "esp_log.h"
#include "common_define.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <time.h>
#include <vector>
#include <string>

static const char* TAG = "MESH_LOG_UTILS";
static const char* LOG_DIR = "/sdcard/logs";

namespace Mesh {

// ============================================================================
// NDJSON parsing
// ============================================================================

static const char* find_field(const char* s, const char* key)
{
    char tok[24];
    int n = snprintf(tok, sizeof(tok), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(tok))
        return nullptr;
    const char* p = strstr(s, tok);
    return p ? p + n : nullptr;
}

static uint32_t f_u32(const char* s, const char* key, uint32_t def = 0)
{
    const char* p = find_field(s, key);
    return p ? (uint32_t)strtoul(p, nullptr, 10) : def;
}

static int f_int(const char* s, const char* key, int def = 0)
{
    const char* p = find_field(s, key);
    return p ? (int)strtol(p, nullptr, 10) : def;
}

static float f_float(const char* s, const char* key, float def = 0.0f)
{
    const char* p = find_field(s, key);
    return p ? strtof(p, nullptr) : def;
}

static bool f_bool(const char* s, const char* key)
{
    const char* p = find_field(s, key);
    return p && strncmp(p, "true", 4) == 0;
}

static uint32_t f_hexid(const char* s, const char* key)
{
    const char* p = find_field(s, key);
    if (!p)
        return 0;
    if (*p == '"')
        p++;
    if (*p == '!')
        p++;
    return (uint32_t)strtoul(p, nullptr, 16);
}

static void f_str(const char* s, const char* key, char* out, size_t cap)
{
    out[0] = '\0';
    const char* p = find_field(s, key);
    if (!p || *p != '"')
        return;
    p++; // opening quote
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap)
    {
        if (*p == '\\' && p[1])
        {
            p++;
            char c = *p;
            if (c == 'n' || c == 't' || c == 'r')
                c = ' ';
            out[o++] = c;
        }
        else
            out[o++] = *p;
        p++;
    }
    out[o] = '\0';
}

bool parse_log_line(const char* line, LogEntry& e)
{
    const char* type = find_field(line, "type");
    if (!type || strncmp(type, "\"pkt\"", 5) != 0)
        return false;

    PacketLogEntry& p = e.pkt;
    memset(&p, 0, sizeof(p));
    e.epoch = f_u32(line, "t", 0);
    p.timestamp_ms = f_u32(line, "ms", 0);

    const char* dir = find_field(line, "dir");
    p.is_tx = dir && strncmp(dir, "\"tx\"", 4) == 0;

    p.from = f_hexid(line, "from");
    p.to = f_hexid(line, "to");
    p.id = f_u32(line, "id");
    p.port = (uint8_t)f_u32(line, "port");
    p.size = (uint16_t)f_u32(line, "size");
    p.rssi = (int16_t)f_int(line, "rssi");
    p.snr = f_float(line, "snr");
    p.hop_start = (uint8_t)f_u32(line, "hop_start");
    p.hop_limit = (uint8_t)f_u32(line, "hop_limit");
    p.channel = (uint8_t)f_u32(line, "ch");
    p.relay_node = (uint8_t)f_u32(line, "relay");
    p.want_ack = f_bool(line, "want_ack");
    p.via_mqtt = f_bool(line, "via_mqtt");
    p.decoded = f_bool(line, "decoded");
    p.crc_error = f_bool(line, "crc_err");
    f_str(line, "desc", p.payload_desc, sizeof(p.payload_desc));
    return true;
}

void load_recent_history(uint32_t window_seconds)
{
    ESP_LOGI(TAG, "Loading recent history (window: %lu s)...", (unsigned long)window_seconds);

    uint32_t now = (uint32_t)time(nullptr);
    if (now < 1700000000u) {
        ESP_LOGW(TAG, "System time not synced, skipping historical log load");
        return;
    }
    uint32_t cutoff = now - window_seconds;

    DIR* d = opendir(LOG_DIR);
    if (!d) return;

    std::vector<std::string> log_files;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strstr(ent->d_name, ".ndjson")) {
            log_files.push_back(ent->d_name);
        }
    }
    closedir(d);

    // Sort log files by name (which contains date/seq) ascending to process oldest first
    std::sort(log_files.begin(), log_files.end());

    auto& ds = MeshDataStore::getInstance();

    // For channel activity tracking
    struct ActivityBucket {
        uint32_t minute_epoch;
        uint32_t count;
    };
    std::vector<ActivityBucket> activity;

    for (const auto& fname : log_files) {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, fname.c_str());

        // Quick check: if the file was last modified before cutoff, skip it
        struct stat st;
        if (stat(path, &st) == 0 && (uint32_t)st.st_mtime < cutoff) {
            continue;
        }

        FILE* f = fopen(path, "r");
        if (!f) continue;

        char line[512];
        while (fgets(line, sizeof(line), f)) {
            LogEntry e;
            if (parse_log_line(line, e)) {
                if (e.epoch < cutoff) continue;

                // 1. Back-fill live packet log (limited to its size)
                ds.addPacketLogEntry(e.pkt, true);

                // 2. Aggregate channel activity (packets per minute)
                uint32_t min_epoch = (e.epoch / 60) * 60;
                bool found = false;
                for (auto& b : activity) {
                    if (b.minute_epoch == min_epoch) {
                        b.count++;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    activity.push_back({min_epoch, 1});
                }

                // 3. Back-fill RSSI history
                if (!e.pkt.is_tx && !e.pkt.crc_error && e.pkt.from != 0) {
                    // Convert epoch to relative millis for the graph
                    uint32_t rel_ms = (uint32_t)millis() - (now - e.epoch) * 1000;
                    ds.addRssiPoint(e.pkt.from, e.pkt.rssi, rel_ms);
                }
            }
        }
        fclose(f);
    }

    // Sort and add activity points
    std::sort(activity.begin(), activity.end(), [](const ActivityBucket& a, const ActivityBucket& b) {
        return a.minute_epoch < b.minute_epoch;
    });
    for (const auto& b : activity) {
        uint32_t rel_ms = (uint32_t)millis() - (now - b.minute_epoch) * 1000;
        ds.addChannelActivityPoint((float)b.count, rel_ms);
    }

    ESP_LOGI(TAG, "Historical load complete.");
}

} // namespace Mesh
