/**
 * @file app_log_reader.cpp
 * @brief Offline NDJSON mesh-log reader. Cloned from app_monitor's packet
 *        list/detail views; data source swapped from the live RAM ring buffer
 *        to records parsed from the .ndjson files under /sdcard/logs.
 */
#include "app_log_reader.h"
#include "common_define.h"
#include "apps/utils/theme/theme_define.h"
#include "esp_log.h"
#include "meshtastic/portnums.pb.h"
#include "mesh/node_db.h"
#include "mesh/mesh_service.h"
#include "apps/utils/ui/draw_helper.h"
#include "apps/utils/ui/key_repeat.h"
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>

static const char* TAG = "APP_LOG_READER";

#define LIST_ITEM_HEIGHT 14
#define SCROLL_BAR_WIDTH 4
#define SCROLLBAR_MIN_HEIGHT 10

static const char* LOG_DIR = "/sdcard/logs";

// Memory/time budget (no PSRAM): only tail-read the file and keep the newest N.
static const long TAIL_BYTES = 256L * 1024L; // bytes from EOF to scan
static const size_t MAX_ENTRIES = 300;       // ~27KB of ReaderEntry (no PSRAM)
static const size_t MAX_FILES = 128;

static const char* HINT_FILES = "[↑][↓] [ENTER] [ESC]";
static const char* HINT_LIST = "[CTRL] [↑][↓][←][→] [ENTER] [ESC]";
static const char* HINT_DETAIL = "[↑][↓][←][→] [ESC]";

using namespace MOONCAKE::APPS;

// ============================================================================
// NDJSON parsing (fixed machine-generated format from MeshLogger::logEntry)
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

// Reads a "!%08x" node id field.
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

// Reads a quoted string value, unescaping the few sequences json_escape emits.
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

// Parse one NDJSON line into a ReaderEntry. Returns false for non-pkt lines.
static bool parse_line(const char* line, AppLogReader::ReaderEntry& e)
{
    const char* type = find_field(line, "type");
    if (!type || strncmp(type, "\"pkt\"", 5) != 0)
        return false; // skip pos track points / unknown

    Mesh::PacketLogEntry& p = e.pkt;
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

// ============================================================================
// File handling
// ============================================================================

void AppLogReader::_scan_files()
{
    _data.files.clear();
    DIR* d = opendir(LOG_DIR);
    if (!d)
        return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr && _data.files.size() < MAX_FILES)
    {
        const char* nm = ent->d_name;
        size_t len = strlen(nm);
        if (len < 8 || strcmp(nm + len - 7, ".ndjson") != 0)
            continue;
        FileItem item;
        item.name = nm;
        item.size = 0;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, nm);
        struct stat st;
        if (stat(path, &st) == 0)
            item.size = (uint32_t)st.st_size;
        _data.files.push_back(std::move(item));
    }
    closedir(d);
    // Names embed the epoch (mesh-<epoch>-<seq>.ndjson) -> sort desc = newest first.
    std::sort(_data.files.begin(),
              _data.files.end(),
              [](const FileItem& a, const FileItem& b) { return a.name > b.name; });
}

bool AppLogReader::_load_file(const std::string& name)
{
    _data.entries.clear();

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, name.c_str());
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGW(TAG, "open failed: %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    long start = (fsize > TAIL_BYTES) ? (fsize - TAIL_BYTES) : 0;
    fseek(f, start, SEEK_SET);

    char line[512];
    // If we seeked into the middle of a line, drop the partial first line.
    if (start > 0)
        (void)fgets(line, sizeof(line), f);

    // Reserve proportional to the bytes we'll actually scan, not the worst case.
    // sizeof(ReaderEntry) is ~92B, so an unconditional MAX_ENTRIES reserve asks for
    // ~60KB of contiguous internal DRAM (no PSRAM here) on every load regardless of
    // file size -> bad_alloc -> abort. NDJSON pkt lines run ~150-250B; /64 is a safely
    // generous line-count estimate that still caps at the trim threshold.
    long scan = fsize - start;
    size_t est = (size_t)(scan / 64) + 16;
    if (est > MAX_ENTRIES + 64)
        est = MAX_ENTRIES + 64;
    _data.entries.reserve(est);
    while (fgets(line, sizeof(line), f))
    {
        ReaderEntry e;
        if (!parse_line(line, e))
            continue;
        _data.entries.push_back(e);
        // Keep only the newest MAX_ENTRIES; trim in batches to amortize.
        if (_data.entries.size() > MAX_ENTRIES + 64)
            _data.entries.erase(_data.entries.begin(), _data.entries.begin() + 64);
    }
    fclose(f);
    if (_data.entries.size() > MAX_ENTRIES)
        _data.entries.erase(_data.entries.begin(), _data.entries.end() - MAX_ENTRIES);

    ESP_LOGI(TAG, "loaded %u entries from %s (tail %ld of %ld)",
             (unsigned)_data.entries.size(), name.c_str(), fsize - start, fsize);
    return true;
}

// ============================================================================
// Lifecycle
// ============================================================================

void AppLogReader::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.view_state = ViewState::FILE_LIST;
    _data.file_index = 0;
    _data.file_scroll = 0;
    _data.update_files = true;
    _data.selected_index = 0;
    _data.scroll_offset = 0;
    _data.update_list = true;
    _data.packet_list_ctrl = false;
    _data.detail_scroll = 0;
    _data.detail_scroll_max = 0;

    hl_text_init(&_data.hint_hl_ctx, _data.hal->canvas(), 20, 1500);
}

void AppLogReader::onResume()
{
    ANIM_APP_OPEN();
    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas()->setFont(FONT_12);
    _data.hal->canvas()->setTextSize(1);
    _data.hal->canvas_update();

    _data.view_state = ViewState::FILE_LIST;
    _data.file_index = 0;
    _data.file_scroll = 0;
    _data.update_files = true;
    _scan_files();
}

void AppLogReader::onRunning()
{
    bool updated = false;
    switch (_data.view_state)
    {
    case ViewState::FILE_LIST:
        updated |= _render_file_list();
        updated |= _render_file_hint();
        if (updated)
            _data.hal->canvas_update();
        _handle_file_input();
        break;

    case ViewState::PACKET_LIST:
        updated |= _render_packet_list();
        updated |= _render_list_hint();
        if (updated)
            _data.hal->canvas_update();
        _handle_list_input();
        break;

    case ViewState::PACKET_DETAIL:
        updated |= _render_packet_detail();
        updated |= _render_detail_hint();
        if (updated)
            _data.hal->canvas_update();
        _handle_detail_input();
        break;
    }
}

void AppLogReader::onDestroy() { hl_text_free(&_data.hint_hl_ctx); }

// ============================================================================
// Helpers (identical mapping to app_monitor)
// ============================================================================

const char* AppLogReader::_port_name(uint8_t port)
{
    switch (port)
    {
    case meshtastic_PortNum_TEXT_MESSAGE_APP:
        return "TEXT";
    case meshtastic_PortNum_POSITION_APP:
        return "POS";
    case meshtastic_PortNum_NODEINFO_APP:
        return "NODE";
    case meshtastic_PortNum_TELEMETRY_APP:
        return "TELE";
    case meshtastic_PortNum_ROUTING_APP:
        return "ROUT";
    case meshtastic_PortNum_ADMIN_APP:
        return "ADMN";
    case meshtastic_PortNum_TRACEROUTE_APP:
        return "TRAC";
    case meshtastic_PortNum_WAYPOINT_APP:
        return "WAPT";
    case meshtastic_PortNum_NEIGHBORINFO_APP:
        return "NEIG";
    case meshtastic_PortNum_STORE_FORWARD_APP:
        return "S&F";
    case meshtastic_PortNum_RANGE_TEST_APP:
        return "RNGE";
    case meshtastic_PortNum_MAP_REPORT_APP:
        return "MAP";
    case meshtastic_PortNum_DETECTION_SENSOR_APP:
        return "SENS";
    case meshtastic_PortNum_REMOTE_HARDWARE_APP:
        return "HWRD";
    case meshtastic_PortNum_ATAK_PLUGIN:
        return "ATAK";
    case meshtastic_PortNum_SERIAL_APP:
        return "SERL";
    case meshtastic_PortNum_PAXCOUNTER_APP:
        return "PAX";
    case 0:
        return "";
    default:
        return nullptr;
    }
}

const char* AppLogReader::_direction_str(const Mesh::PacketLogEntry& pkt) { return pkt.is_tx ? "TX>" : "RX<"; }

uint32_t AppLogReader::_direction_color(const Mesh::PacketLogEntry& pkt)
{
    return lgfx::v1::convert_to_rgb888(pkt.is_tx ? TFT_GREEN : TFT_CYAN);
}

static uint32_t packet_id_color(uint32_t id)
{
    uint8_t r = (uint8_t)((id * 37) ^ (id >> 8));
    uint8_t g = (uint8_t)((id * 59) ^ (id >> 16));
    uint8_t b = (uint8_t)((id * 101) ^ (id >> 24));
    r = 40 + (r % 80);
    g = 40 + (g % 80);
    b = 40 + (b % 80);
    return (r << 16) | (g << 8) | b;
}

static void format_inter_packet_gap(char* buf, size_t buflen, uint32_t delta_ms)
{
    if (delta_ms < 1000u)
        snprintf(buf, buflen, "+%lums", (unsigned long)delta_ms);
    else if (delta_ms < 60u * 1000)
        snprintf(buf, buflen, "+%lus", (unsigned long)(delta_ms / 1000u));
    else if (delta_ms < 60u * 60 * 1000)
        snprintf(buf, buflen, "+%lum", (unsigned long)(delta_ms / (60u * 1000)));
    else if (delta_ms < 24u * 60 * 60 * 1000)
        snprintf(buf, buflen, "+%luh", (unsigned long)(delta_ms / (60u * 60 * 1000u)));
    else
    {
        unsigned long d = delta_ms / (24u * 60 * 60 * 1000);
        if (d > 999ul)
            d = 999;
        snprintf(buf, buflen, "+%lud", d);
    }
}

// Render a timestamp from an absolute epoch (logs carry it; live monitor can't).
static void format_epoch_time(char* buf, size_t buflen, uint32_t epoch)
{
    if (epoch <= 1u)
    {
        snprintf(buf, buflen, "--:--:--");
        return;
    }
    time_t t = (time_t)epoch;
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    snprintf(buf, buflen, "%02d:%02d:%02d", tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
}

// ============================================================================
// File List Rendering
// ============================================================================

bool AppLogReader::_render_file_list()
{
    if (!_data.update_files)
        return false;
    _data.update_files = false;

    auto* canvas = _data.hal->canvas();
    canvas->fillScreen(THEME_COLOR_BG);
    canvas->setFont(FONT_12);

    // Header
    canvas->setTextColor(TFT_ORANGE);
    canvas->drawString("Mesh logs", 4, 0);
    canvas->drawFastHLine(0, 14, canvas->width(), THEME_COLOR_HEADER_LINE);

    int total = (int)_data.files.size();
    if (total == 0)
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("<no .ndjson logs>", canvas->width() / 2, canvas->height() / 2 - 6);
        return true;
    }

    if (_data.file_index >= total)
        _data.file_index = total - 1;
    if (_data.file_index < 0)
        _data.file_index = 0;

    const int item_y_start = 16;
    const int max_visible = (canvas->height() - item_y_start - 9) / (LIST_ITEM_HEIGHT + 1);

    if (_data.file_index < _data.file_scroll)
        _data.file_scroll = _data.file_index;
    if (_data.file_index >= _data.file_scroll + max_visible)
        _data.file_scroll = _data.file_index - max_visible + 1;

    int y = item_y_start;
    for (int i = 0; i < max_visible && (_data.file_scroll + i) < total; i++)
    {
        int idx = _data.file_scroll + i;
        const auto& it = _data.files[idx];
        bool selected = (idx == _data.file_index);

        if (selected)
            canvas->fillRect(2, y, canvas->width() - 4, LIST_ITEM_HEIGHT, THEME_COLOR_BG_SELECTED);

        canvas->setTextColor(selected ? THEME_COLOR_SELECTED : THEME_COLOR_UNSELECTED);
        canvas->drawString(it.name.c_str(), 4, y + 1);

        char size_buf[16];
        if (it.size >= 1024u * 1024u)
            snprintf(size_buf, sizeof(size_buf), "%.1fM", it.size / (1024.0 * 1024.0));
        else if (it.size >= 1024u)
            snprintf(size_buf, sizeof(size_buf), "%uK", (unsigned)(it.size / 1024u));
        else
            snprintf(size_buf, sizeof(size_buf), "%uB", (unsigned)it.size);
        canvas->setTextColor(selected ? THEME_COLOR_SELECTED : TFT_DARKGREY);
        canvas->drawRightString(size_buf, canvas->width() - SCROLL_BAR_WIDTH - 4, y + 1);

        y += LIST_ITEM_HEIGHT + 1;
    }

    UTILS::UI::draw_scrollbar(canvas,
                              canvas->width() - SCROLL_BAR_WIDTH - 1,
                              item_y_start,
                              SCROLL_BAR_WIDTH,
                              max_visible * (LIST_ITEM_HEIGHT + 1),
                              total,
                              max_visible,
                              _data.file_scroll,
                              SCROLLBAR_MIN_HEIGHT);
    return true;
}

bool AppLogReader::_render_file_hint()
{
    return hl_text_render(&_data.hint_hl_ctx,
                          HINT_FILES,
                          0,
                          _data.hal->canvas()->height() - 9,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

// ============================================================================
// Packet List Rendering (ported from app_monitor; data source = _data.entries)
// ============================================================================

bool AppLogReader::_render_packet_list()
{
    if (!_data.update_list)
        return false;
    _data.update_list = false;

    auto* canvas = _data.hal->canvas();
    canvas->fillScreen(THEME_COLOR_BG);
    canvas->setFont(FONT_12);

    int total = (int)_data.entries.size();
    if (total == 0)
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("<no packets>", canvas->width() / 2, canvas->height() / 2 - 6);
        return true;
    }

    if (_data.selected_index >= total)
        _data.selected_index = total - 1;
    if (_data.selected_index < 0)
        _data.selected_index = 0;

    const int item_y_start = 0;
    const int max_visible = (canvas->height() - item_y_start - 9) / (LIST_ITEM_HEIGHT + 1);

    if (_data.selected_index < _data.scroll_offset)
        _data.scroll_offset = _data.selected_index;
    if (_data.selected_index >= _data.scroll_offset + max_visible)
        _data.scroll_offset = _data.selected_index - max_visible + 1;

    uint32_t our_id = _data.hal->mesh() ? _data.hal->mesh()->getNodeId() : 0;
    int y = item_y_start;
    for (int i = 0; i < max_visible && (_data.scroll_offset + i) < total; i++)
    {
        int idx = _data.scroll_offset + i;
        // [0]=oldest, [total-1]=newest; display newest at top.
        const auto& pkt = _data.entries[total - 1 - idx].pkt;
        bool selected = (idx == _data.selected_index);

        uint32_t bg = selected ? THEME_COLOR_BG_SELECTED : THEME_COLOR_BG;
        uint32_t fg = selected ? THEME_COLOR_SELECTED : THEME_COLOR_UNSELECTED;

        if (selected)
            canvas->fillRect(2, y, canvas->width() - 4, LIST_ITEM_HEIGHT, THEME_COLOR_BG_SELECTED);
        bool involves_us = pkt.is_tx ? (pkt.from == our_id) : (pkt.to == our_id || pkt.to == 0xFFFFFFFF);
        uint32_t direction_color = _direction_color(pkt);
        canvas->setTextColor(selected ? THEME_COLOR_SELECTED : direction_color);
        canvas->drawString(_direction_str(pkt), 4, y + 1);

        const char* pname = nullptr;
        char port_buf[6];
        if (pkt.crc_error)
            pname = "CRC";
        else
        {
            pname = _port_name(pkt.port);
            if (!pname)
            {
                snprintf(port_buf, sizeof(port_buf), "P%02X", pkt.port);
                pname = port_buf;
            }
        }
        uint32_t id_bg = pkt.crc_error ? THEME_COLOR_SIGNAL_NONE : (pkt.port == 0 ? bg : packet_id_color(pkt.id));
        canvas->fillRect(27, y, 29, LIST_ITEM_HEIGHT, id_bg);
        canvas->setTextColor(fg);
        canvas->drawCenterString(pname, 28 + (28 - 2) / 2, y + 1);

        bool known_from = pkt.is_tx ? (pkt.from == our_id) : false;
        uint32_t nc = UTILS::UI::node_color(pkt.from);
        uint32_t ntc = UTILS::UI::node_text_color(pkt.from);
        std::string from_label;
        Mesh::NodeInfo ni;
        if (pkt.crc_error && pkt.from == 0)
            from_label = "????";
        else if (_data.hal->mesh() && _data.hal->mesh()->getNode(pkt.from, ni))
        {
            known_from = true;
            from_label = Mesh::NodeDB::getLabel(ni);
        }
        else
            from_label = std::format("{:04x}", (unsigned)(pkt.from & 0xFFFF));
        if (!known_from)
        {
            nc = THEME_COLOR_BG_SELECTED_DARK;
            ntc = THEME_COLOR_SELECTED;
        }
        int pill_w = 4 * 6 + 4;
        int pill_x = 60;
        canvas->fillRoundRect(pill_x, y, pill_w, LIST_ITEM_HEIGHT, 4, nc);
        canvas->setTextColor(ntc);
        canvas->drawCenterString(from_label.c_str(), pill_x + pill_w / 2, y + 1);
        uint32_t arrow_color = involves_us ? direction_color : lgfx::v1::convert_to_rgb888(TFT_ORANGE);
        canvas->setTextColor(selected ? THEME_COLOR_SELECTED : arrow_color);
        canvas->drawString("→", pill_x + pill_w + 1, y + 1);

        bool known_to = pkt.is_tx ? false : (pkt.to == our_id);
        int pill2_x = pill_x + pill_w + 10;
        uint32_t nc2 = UTILS::UI::node_color(pkt.to);
        uint32_t ntc2 = UTILS::UI::node_text_color(pkt.to);
        std::string to_label;
        if (pkt.crc_error && pkt.to == 0)
            to_label = "????";
        else if (pkt.to == 0xFFFFFFFF)
            to_label = "→→→";
        else if (_data.hal->mesh() && _data.hal->mesh()->getNode(pkt.to, ni))
        {
            known_to = true;
            to_label = Mesh::NodeDB::getLabel(ni);
        }
        else
            to_label = std::format("{:04x}", (unsigned)(pkt.to & 0xFFFF));
        if (!known_to)
        {
            nc2 = THEME_COLOR_BG_SELECTED_DARK;
            ntc2 = THEME_COLOR_SELECTED;
        }
        canvas->fillRoundRect(pill2_x, y, pill_w, LIST_ITEM_HEIGHT, 4, nc2);
        canvas->setTextColor(ntc2);
        canvas->drawCenterString(to_label.c_str(), pill2_x + pill_w / 2, y + 1);

        if (_data.packet_list_ctrl)
        {
            char gap_buf[16];
            uint32_t delta_ms = 0;
            if (idx + 1 < total)
            {
                const auto& older = _data.entries[total - 1 - (idx + 1)].pkt;
                delta_ms = pkt.timestamp_ms - older.timestamp_ms;
            }
            else
                delta_ms = pkt.timestamp_ms;
            format_inter_packet_gap(gap_buf, sizeof(gap_buf), delta_ms);

            const int pill_w_rel = 4 * 6 + 4;
            const int rhs = canvas->width() - 6;
            const int gap_slot = 5 * 6;
            int relay_x = pill2_x + pill_w + 2;

            canvas->setTextColor(selected ? THEME_COLOR_SELECTED : direction_color);
            canvas->drawRightString(gap_buf, rhs, y + 1);

            if (!(pkt.is_tx || pkt.relay_node == 0))
            {
                uint32_t relay_id = _data.hal->nodedb() ? _data.hal->nodedb()->findNodeByRelayByte(pkt.relay_node) : 0;
                bool known_rel = relay_id != 0 && _data.hal->mesh() && _data.hal->mesh()->getNode(relay_id, ni);
                std::string rel_label =
                    known_rel ? Mesh::NodeDB::getLabel(ni) : std::format("{:02x}", (unsigned)pkt.relay_node);
                uint32_t nc_r = known_rel ? UTILS::UI::node_color(relay_id) : UTILS::UI::node_color((uint32_t)pkt.relay_node);
                uint32_t ntc_r =
                    known_rel ? UTILS::UI::node_text_color(relay_id) : UTILS::UI::node_text_color((uint32_t)pkt.relay_node);
                if (!known_rel)
                {
                    nc_r = THEME_COLOR_BG_SELECTED_DARK;
                    ntc_r = THEME_COLOR_SELECTED;
                }
                canvas->fillRoundRect(relay_x, y, pill_w_rel, LIST_ITEM_HEIGHT, 4, nc_r);
                canvas->setTextColor(ntc_r);
                canvas->drawCenterString(rel_label.c_str(), relay_x + pill_w_rel / 2, y + 1);
            }

            // Absolute wall-clock time from the logged epoch.
            char time_buf[10];
            format_epoch_time(time_buf, sizeof(time_buf), _data.entries[total - 1 - idx].epoch);
            canvas->setTextColor(selected ? THEME_COLOR_SELECTED : direction_color);
            canvas->drawRightString(time_buf, rhs - gap_slot, y + 1);
        }
        else
        {
            std::string hop_str;
            if (pkt.hop_start > 0)
            {
                int hops_used = pkt.hop_start - pkt.hop_limit;
                hop_str = std::format("{:d}/{:d}", hops_used, pkt.hop_start);
            }
            else
                hop_str = std::format("[{:d}]", pkt.hop_limit);
            canvas->setTextColor(selected ? THEME_COLOR_SELECTED : direction_color);
            canvas->drawString(hop_str.c_str(), pill2_x + pill_w + 2, y + 1);

            if (pkt.channel != 0)
            {
                std::string channel_str = std::format("#{:02X}", pkt.channel);
                canvas->setTextColor(selected ? THEME_COLOR_SELECTED : direction_color);
                canvas->drawRightString(channel_str.c_str(), canvas->width() - 68, y + 1);
            }
            std::string size_str = std::format("{:d}B", pkt.size);
            canvas->setTextColor(selected ? THEME_COLOR_SELECTED : direction_color);
            canvas->drawRightString(size_str.c_str(), canvas->width() - 38, y + 1);

            if (!pkt.is_tx && (pkt.snr != 0.0f || pkt.rssi != 0))
            {
                int sig_x = canvas->width() - 6;
                canvas->setFont(FONT_6);

                char snr_buf[10];
                snprintf(snr_buf, sizeof(snr_buf), "%.1f", pkt.snr);
                uint32_t snr_color = (pkt.snr > -7.5f)     ? THEME_COLOR_SIGNAL_GOOD
                                     : (pkt.snr > -13.0f)  ? THEME_COLOR_SIGNAL_FAIR
                                     : (pkt.snr >= -15.0f) ? THEME_COLOR_SIGNAL_BAD
                                                           : THEME_COLOR_SIGNAL_NONE;
                canvas->setTextColor(selected ? THEME_COLOR_SELECTED : snr_color);
                canvas->drawRightString(snr_buf, sig_x, y + 1);

                char rssi_buf[10];
                snprintf(rssi_buf, sizeof(rssi_buf), "%d", (int)pkt.rssi);
                uint32_t rssi_color = (pkt.rssi > -115)   ? THEME_COLOR_SIGNAL_GOOD
                                      : (pkt.rssi > -120) ? THEME_COLOR_SIGNAL_FAIR
                                      : (pkt.rssi > -126) ? THEME_COLOR_SIGNAL_BAD
                                                          : THEME_COLOR_SIGNAL_NONE;
                canvas->setTextColor(selected ? THEME_COLOR_SELECTED : rssi_color);
                canvas->drawRightString(rssi_buf, sig_x, y + 8);

                canvas->setFont(FONT_12);
            }
        }

        y += LIST_ITEM_HEIGHT + 1;
    }

    UTILS::UI::draw_scrollbar(canvas,
                              canvas->width() - SCROLL_BAR_WIDTH - 1,
                              item_y_start,
                              SCROLL_BAR_WIDTH,
                              max_visible * (LIST_ITEM_HEIGHT + 1),
                              total,
                              max_visible,
                              _data.scroll_offset,
                              SCROLLBAR_MIN_HEIGHT);
    return true;
}

bool AppLogReader::_render_list_hint()
{
    return hl_text_render(&_data.hint_hl_ctx,
                          HINT_LIST,
                          0,
                          _data.hal->canvas()->height() - 9,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

// ============================================================================
// Packet Detail Rendering (ported from app_monitor)
// ============================================================================

bool AppLogReader::_render_packet_detail()
{
    if (!_data.update_list)
        return false;
    _data.update_list = false;

    auto* canvas = _data.hal->canvas();
    canvas->fillScreen(THEME_COLOR_BG);
    canvas->setFont(FONT_12);

    const auto& pkt = _data.detail_pkt;

    canvas->setTextColor(TFT_ORANGE);
    canvas->drawString("<", 2, 0);
    canvas->drawString("Packet detail", 14, 0);
    canvas->setTextColor(_direction_color(pkt));
    canvas->drawRightString(_direction_str(pkt), canvas->width() - 2, 0);
    canvas->drawFastHLine(0, 14, canvas->width(), THEME_COLOR_HEADER_LINE);

    struct DetailRow
    {
        const char* label;
        char value[32];
        uint32_t value_color;
        bool is_header;
    };

    DetailRow rows[28];
    int row_count = 0;

    auto add_row = [&](const char* label, const char* val, uint32_t color = THEME_COLOR_UNSELECTED)
    {
        if (row_count < 28)
        {
            rows[row_count].label = label;
            strncpy(rows[row_count].value, val, 31);
            rows[row_count].value[31] = '\0';
            rows[row_count].value_color = color;
            rows[row_count].is_header = false;
            row_count++;
        }
    };
    auto add_header = [&](const char* label)
    {
        if (row_count < 28)
        {
            rows[row_count].label = label;
            rows[row_count].value[0] = '\0';
            rows[row_count].value_color = lgfx::v1::convert_to_rgb888(TFT_ORANGE);
            rows[row_count].is_header = true;
            row_count++;
        }
    };

    if (pkt.crc_error)
        add_row("Status", "CRC error (corrupted)", (uint32_t)THEME_COLOR_SIGNAL_NONE);
    {
        char buf[32];
        Mesh::NodeInfo ni;
        if (pkt.crc_error && pkt.from == 0)
            snprintf(buf, sizeof(buf), "unknown (header corrupt)");
        else if (_data.hal->mesh() && _data.hal->mesh()->getNode(pkt.from, ni) && ni.info.user.short_name[0])
            snprintf(buf, sizeof(buf), "%s (!%08lx)", ni.info.user.short_name, (unsigned long)pkt.from);
        else
            snprintf(buf, sizeof(buf), "!%08lx", (unsigned long)pkt.from);
        add_row("From", buf);
    }
    {
        char buf[32];
        Mesh::NodeInfo ni;
        if (pkt.crc_error && pkt.to == 0)
            snprintf(buf, sizeof(buf), "unknown (header corrupt)");
        else if (pkt.to == 0xFFFFFFFF)
            snprintf(buf, sizeof(buf), "BROADCAST");
        else if (_data.hal->mesh() && _data.hal->mesh()->getNode(pkt.to, ni) && ni.info.user.short_name[0])
            snprintf(buf, sizeof(buf), "%s (!%08lx)", ni.info.user.short_name, (unsigned long)pkt.to);
        else
            snprintf(buf, sizeof(buf), "!%08lx", (unsigned long)pkt.to);
        add_row("To", buf);
    }
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)pkt.id);
        add_row("ID", buf);
    }
    {
        char buf[32];
        format_epoch_time(buf, sizeof(buf), _data.detail_epoch);
        add_row("Time", buf);
    }
    {
        char buf[32];
        format_inter_packet_gap(buf, sizeof(buf), _data.detail_delta_ms);
        add_row("Delta", buf);
    }
    if (pkt.relay_node != 0)
    {
        char buf[32];
        uint32_t relay_id = _data.hal->nodedb() ? _data.hal->nodedb()->findNodeByRelayByte(pkt.relay_node) : 0;
        const auto* relay_idx = _data.hal->nodedb() ? _data.hal->nodedb()->getNodeIndex(relay_id) : nullptr;
        if (relay_idx)
            snprintf(buf, sizeof(buf), "#%02x → %s (!%08lx)", pkt.relay_node, relay_idx->short_name,
                     (unsigned long)relay_id);
        else
            snprintf(buf, sizeof(buf), "#%02x", (unsigned)pkt.relay_node);
        add_row("Relay", buf);
    }
    {
        const char* pname = pkt.crc_error ? "CRC" : _port_name(pkt.port);
        char buf[16];
        if (pname)
            snprintf(buf, sizeof(buf), "%s (%d)", pname, pkt.port);
        else
            snprintf(buf, sizeof(buf), "%d", pkt.port);
        add_row("Port", buf,
                pkt.crc_error ? (uint32_t)THEME_COLOR_SIGNAL_NONE
                              : lgfx::v1::convert_to_rgb888((pkt.decoded ? TFT_WHITE : TFT_DARKGREY)));
    }
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d bytes", pkt.size);
        add_row("Size", buf);
    }
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02X", pkt.channel);
        add_row("Channel", buf);
    }
    {
        char buf[16];
        if (pkt.hop_start > 0)
        {
            int hops_used = pkt.hop_start - pkt.hop_limit;
            snprintf(buf, sizeof(buf), "%d/%d", hops_used, pkt.hop_start);
        }
        else
            snprintf(buf, sizeof(buf), "lim %d", pkt.hop_limit);
        add_row("Hops", buf);
    }
    add_row("WantACK", pkt.want_ack ? "yes" : "no", lgfx::v1::convert_to_rgb888(pkt.want_ack ? TFT_YELLOW : TFT_DARKGREY));
    add_row("Decoded", pkt.decoded ? "yes" : "no", lgfx::v1::convert_to_rgb888(pkt.decoded ? TFT_GREEN : TFT_RED));
    if (!pkt.is_tx)
    {
        add_row("MQTT", pkt.via_mqtt ? "yes" : "no", lgfx::v1::convert_to_rgb888(pkt.via_mqtt ? TFT_CYAN : TFT_DARKGREY));
        char buf[12];
        snprintf(buf, sizeof(buf), "%d dBm", pkt.rssi);
        uint32_t rssi_color;
        if (pkt.rssi > -90)
            rssi_color = THEME_COLOR_SIGNAL_GOOD;
        else if (pkt.rssi > -110)
            rssi_color = THEME_COLOR_SIGNAL_FAIR;
        else if (pkt.rssi > -120)
            rssi_color = THEME_COLOR_SIGNAL_BAD;
        else
            rssi_color = THEME_COLOR_SIGNAL_NONE;
        add_row("RSSI", buf, rssi_color);

        snprintf(buf, sizeof(buf), "%.1f dB", pkt.snr);
        add_row("SNR", buf);
    }

    if (pkt.decoded && !pkt.crc_error)
    {
        add_header(_port_name(pkt.port));
        if (pkt.payload_desc[0])
            add_row("Payload", pkt.payload_desc);
    }

    const int row_height = 14;
    const int y_start = 15;
    const int max_visible = (canvas->height() - y_start - 9) / (row_height + 1);

    int max_scroll = std::max(0, row_count - max_visible);
    _data.detail_scroll_max = max_scroll;
    if (_data.detail_scroll > max_scroll)
        _data.detail_scroll = max_scroll;
    if (_data.detail_scroll < 0)
        _data.detail_scroll = 0;

    int y = y_start;
    for (int i = 0; i < max_visible && (_data.detail_scroll + i) < row_count; i++)
    {
        const auto& row = rows[_data.detail_scroll + i];
        if (row.is_header)
        {
            canvas->drawFastHLine(0, y + row_height / 2, canvas->width(), THEME_COLOR_HEADER_LINE);
            canvas->setTextColor(TFT_ORANGE);
            canvas->drawString(row.label, 4, y + 1);
        }
        else
        {
            canvas->setTextColor(TFT_DARKGREY);
            canvas->drawString(row.label, 4, y + 1);
            canvas->setTextColor(row.value_color);
            canvas->drawString(row.value, 60, y + 1);
        }
        y += row_height + 1;
    }

    UTILS::UI::draw_scrollbar(canvas,
                              canvas->width() - SCROLL_BAR_WIDTH - 1,
                              y_start,
                              SCROLL_BAR_WIDTH,
                              max_visible * (row_height + 1),
                              row_count,
                              max_visible,
                              _data.detail_scroll,
                              SCROLLBAR_MIN_HEIGHT);
    return true;
}

bool AppLogReader::_render_detail_hint()
{
    return hl_text_render(&_data.hint_hl_ctx,
                          HINT_DETAIL,
                          0,
                          _data.hal->canvas()->height() - 9,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

// ============================================================================
// Input Handling
// ============================================================================

void AppLogReader::_handle_file_input()
{
    static bool is_repeat = false;
    static uint32_t next_fire_ts = 0;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = (uint32_t)millis();

        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE) ||
            _data.hal->home_button()->is_pressed())
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
            destroyApp();
            return;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            int total = (int)_data.files.size();
            if (total > 0 && _data.file_index < total)
            {
                _data.hal->playNextSound();
                _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);

                // Loading notice (parse can take a moment on SD).
                auto* canvas = _data.hal->canvas();
                canvas->fillScreen(THEME_COLOR_BG);
                canvas->setFont(FONT_12);
                canvas->setTextColor(TFT_ORANGE);
                canvas->drawCenterString("Loading...", canvas->width() / 2, canvas->height() / 2 - 6);
                _data.hal->canvas_update();

                _load_file(_data.files[_data.file_index].name);
                _data.selected_index = 0;
                _data.scroll_offset = 0;
                _data.packet_list_ctrl = false;
                _data.view_state = ViewState::PACKET_LIST;
                _data.update_list = true;
                hl_text_reset(&_data.hint_hl_ctx);
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            int total = (int)_data.files.size();
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.file_index < total - 1)
            {
                _data.file_index++;
                _data.hal->playNextSound();
                _data.update_files = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.file_index > 0)
            {
                _data.file_index--;
                _data.hal->playNextSound();
                _data.update_files = true;
            }
        }
    }
    else
        is_repeat = false;
}

void AppLogReader::_handle_list_input()
{
    static bool is_repeat = false;
    static uint32_t next_fire_ts = 0;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();
    {
        bool ctrl = _data.hal->keyboard()->keysState().ctrl;
        if (_data.packet_list_ctrl != ctrl)
        {
            _data.packet_list_ctrl = ctrl;
            _data.update_list = true;
        }
    }

    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = (uint32_t)millis();

        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            // Back to file picker.
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
            _data.view_state = ViewState::FILE_LIST;
            _data.update_files = true;
            hl_text_reset(&_data.hint_hl_ctx);
            return;
        }
        else if (_data.hal->home_button()->is_pressed())
        {
            _data.hal->playNextSound();
            destroyApp();
            return;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            int total = (int)_data.entries.size();
            if (total > 0 && _data.selected_index < total)
            {
                _data.hal->playNextSound();
                _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
                const auto& sel = _data.entries[total - 1 - _data.selected_index];
                _data.detail_pkt = sel.pkt;
                _data.detail_epoch = sel.epoch;
                if (_data.selected_index + 1 < total)
                {
                    const auto& older = _data.entries[total - 1 - (_data.selected_index + 1)].pkt;
                    _data.detail_delta_ms = _data.detail_pkt.timestamp_ms - older.timestamp_ms;
                }
                else
                    _data.detail_delta_ms = _data.detail_pkt.timestamp_ms;
                _data.detail_scroll = 0;
                _data.view_state = ViewState::PACKET_DETAIL;
                _data.update_list = true;
                hl_text_reset(&_data.hint_hl_ctx);
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            int total = (int)_data.entries.size();
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index < total - 1)
            {
                _data.selected_index++;
                _data.hal->playNextSound();
                _data.update_list = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index > 0)
            {
                _data.selected_index--;
                _data.hal->playNextSound();
                _data.update_list = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            int total = (int)_data.entries.size();
            int page = (_data.hal->canvas()->height() - 9) / (LIST_ITEM_HEIGHT + 1);
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index < total - 1)
            {
                _data.selected_index = (_data.selected_index + page < total - 1) ? _data.selected_index + page : total - 1;
                _data.hal->playNextSound();
                _data.update_list = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index > 0)
            {
                int page = (_data.hal->canvas()->height() - 9) / (LIST_ITEM_HEIGHT + 1);
                _data.selected_index = (_data.selected_index - page > 0) ? _data.selected_index - page : 0;
                _data.hal->playNextSound();
                _data.update_list = true;
            }
        }
    }
    else
        is_repeat = false;
}

void AppLogReader::_handle_detail_input()
{
    static bool is_repeat = false;
    static uint32_t next_fire_ts = 0;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = (uint32_t)millis();

        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            _data.view_state = ViewState::PACKET_LIST;
            _data.update_list = true;
            hl_text_reset(&_data.hint_hl_ctx);
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.detail_scroll < _data.detail_scroll_max)
            {
                _data.detail_scroll++;
                _data.hal->playNextSound();
                _data.update_list = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.detail_scroll > 0)
            {
                _data.detail_scroll--;
                _data.hal->playNextSound();
                _data.update_list = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.detail_scroll < _data.detail_scroll_max)
            {
                const int page = ((_data.hal->canvas()->height() - 15 - 9) / 15);
                _data.detail_scroll = std::min(_data.detail_scroll + page, _data.detail_scroll_max);
                _data.hal->playNextSound();
                _data.update_list = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.detail_scroll > 0)
            {
                const int page = ((_data.hal->canvas()->height() - 15 - 9) / 15);
                _data.detail_scroll = std::max(_data.detail_scroll - page, 0);
                _data.hal->playNextSound();
                _data.update_list = true;
            }
        }
    }
    else
        is_repeat = false;
}
