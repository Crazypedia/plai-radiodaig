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
#include <map>

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

// --- Back-fill resource budgets (no PSRAM: keep boot fast and heap bounded) ---
// Only scan the tail of an oversized file. A 2h window at moderate traffic is a
// few hundred KB of NDJSON; the current day's file can be up to 16 MB, so
// reading it whole from the start would stall boot on SD I/O.
static const long HISTORY_TAIL_BYTES = 512L * 1024L;
// Hard cap on parsed lines across all files so a pathological log can never make
// boot run unbounded. At ~150-250 B/line this comfortably covers the tail scan.
static const uint32_t HISTORY_MAX_LINES = 40000;
// Clamp the requested window so the activity bucket array stays small.
static const uint32_t HISTORY_MAX_WINDOW_S = 24u * 3600u;

void load_recent_history(uint32_t window_seconds)
{
    if (window_seconds > HISTORY_MAX_WINDOW_S)
        window_seconds = HISTORY_MAX_WINDOW_S;
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

    // Channel activity: one packets-per-minute bucket per minute in the window.
    // Direct-indexed by minute offset from the cutoff so aggregation is O(1) per
    // packet instead of an O(buckets) linear scan.
    const uint32_t base_min = cutoff / 60;
    const size_t bucket_count = (size_t)(window_seconds / 60) + 2;
    std::vector<uint32_t> activity(bucket_count, 0);

    // RSSI back-fill down-sampling: the graph keeps at most one point per node
    // per minute (MAX_GRAPH_POINTS over the window), so record at most one sample
    // per (node, minute). This avoids thousands of push/erase-from-front ops.
    std::map<uint32_t, uint32_t> rssi_last_min;

    uint32_t lines_parsed = 0;
    bool budget_hit = false;

    for (const auto& fname : log_files) {
        if (budget_hit) break;

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, fname.c_str());

        // Quick check: if the file was last modified before cutoff, skip it
        struct stat st;
        if (stat(path, &st) == 0 && (uint32_t)st.st_mtime < cutoff) {
            continue;
        }

        FILE* f = fopen(path, "r");
        if (!f) continue;

        // For an oversized file, seek to the tail: the in-window (recent) records
        // live at the end, so there is no need to read megabytes of old lines.
        char line[512];
        if (st.st_size > HISTORY_TAIL_BYTES) {
            fseek(f, st.st_size - HISTORY_TAIL_BYTES, SEEK_SET);
            (void)fgets(line, sizeof(line), f); // drop the partial first line
        }

        while (fgets(line, sizeof(line), f)) {
            if (++lines_parsed > HISTORY_MAX_LINES) {
                ESP_LOGW(TAG, "History line budget reached (%lu), stopping scan",
                         (unsigned long)HISTORY_MAX_LINES);
                budget_hit = true;
                break;
            }

            LogEntry e;
            if (!parse_log_line(line, e)) continue;
            if (e.epoch < cutoff || e.epoch > now) continue;

            // 1. Back-fill live packet log (bounded by the ring buffer size).
            ds.addPacketLogEntry(e.pkt, true);

            // 2. Aggregate channel activity (packets per minute).
            size_t idx = (size_t)(e.epoch / 60 - base_min);
            if (idx < activity.size())
                activity[idx]++;

            // 3. Back-fill RSSI history, down-sampled to one sample per minute.
            if (!e.pkt.is_tx && !e.pkt.crc_error && e.pkt.from != 0) {
                uint32_t min_epoch = e.epoch / 60;
                auto it = rssi_last_min.find(e.pkt.from);
                if (it == rssi_last_min.end() || it->second != min_epoch) {
                    rssi_last_min[e.pkt.from] = min_epoch;
                    // Same-minute-resolution timestamp keeps back-filled points on
                    // the same relative timeline as later live samples.
                    uint32_t rel_ms = (uint32_t)millis() - (now - e.epoch) * 1000;
                    ds.addRssiPoint(e.pkt.from, e.pkt.rssi, rel_ms);
                }
            }
        }
        fclose(f);
    }

    // Emit activity points in chronological order (oldest first).
    for (size_t i = 0; i < activity.size(); i++) {
        if (activity[i] == 0) continue;
        uint32_t min_epoch = (base_min + i) * 60;
        uint32_t rel_ms = (uint32_t)millis() - (now - min_epoch) * 1000;
        ds.addChannelActivityPoint((float)activity[i], rel_ms);
    }

    ESP_LOGI(TAG, "Historical load complete (%lu lines parsed).", (unsigned long)lines_parsed);
}

} // namespace Mesh
