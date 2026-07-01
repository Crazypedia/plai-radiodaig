/**
 * @file app_node_rogue.cpp
 * @brief Rogue Node Tracker - flags noisy nodes by airtime/rate + collision warnings
 * @version 1.1
 * @date 2026-06-27
 */
#include "app_node_rogue.h"
#include "common_define.h"
#include "apps/utils/theme/theme_define.h"
#include "apps/utils/ui/key_repeat.h"
#include "mesh/mesh_service.h"
#include "mesh/mesh_data.h"
#include "mesh/node_db.h"
#include "mesh/node_threat.h"
#include <algorithm>
#include <math.h>

using namespace MOONCAKE::APPS;

#define TAB_HEIGHT 14
#define HEADER_HEIGHT (TAB_HEIGHT + 11)
#define FOOTER_HEIGHT 27
#define ROW_HEIGHT 15

// Heuristic thresholds for flagging a node as noisy/rogue.
#define ROGUE_SHARE_PCT 35.0f // % of observed channel airtime
#define WARN_SHARE_PCT 20.0f
#define ROGUE_RATE_PPM 15.0f // packets per minute from one node
#define WARN_RATE_PPM 8.0f

#define REFRESH_INTERVAL_MS 1000

static const char* HINT = "[←][→] [↑][↓] [ESC]";

// LoRa time-on-air (ms) for an explicit-header, CRC-on packet.
static uint32_t lora_toa_ms(uint16_t payload_len, int sf, int cr_denom, float bw_khz)
{
    if (sf < 6) sf = 6;
    if (sf > 12) sf = 12;
    double bw = (double)bw_khz * 1000.0;
    if (bw <= 0.0) bw = 250000.0;
    int cr = cr_denom;
    if (cr < 5) cr += 4;
    if (cr > 8) cr = 8;

    double tsym = (double)(1u << sf) / bw;
    int de = (tsym > 0.016) ? 1 : 0;
    const int preamble = 16, crc = 1, ih = 0;

    double t_preamble = (preamble + 4.25) * tsym;
    double num = 8.0 * payload_len - 4.0 * sf + 28.0 + 16.0 * crc - 20.0 * ih;
    double den = 4.0 * (sf - 2 * de);
    double payload_symb = 8.0;
    if (num > 0.0 && den > 0.0)
        payload_symb += ceil(num / den) * (double)cr;

    double toa = t_preamble + payload_symb * tsym;
    return (uint32_t)(toa * 1000.0 + 0.5);
}

static float haversine_m(float lat1, float lon1, float lat2, float lon2)
{
    float dlat = (lat2 - lat1) * M_PI / 180.0f;
    float dlon = (lon2 - lon1) * M_PI / 180.0f;
    float a = sinf(dlat / 2) * sinf(dlat / 2) +
              cosf(lat1 * M_PI / 180.0f) * cosf(lat2 * M_PI / 180.0f) *
              sinf(dlon / 2) * sinf(dlon / 2);
    float c = 2 * atan2f(sqrtf(a), sqrtf(1 - a));
    return 6371000.0f * c;
}

void AppNodeRogue::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.current_tab = Tab::TRAFFIC;
    _data.selected_index = 0;
    _data.scroll_offset = 0;
    _data.last_generation = 0xFFFFFFFF;
    _data.last_refresh_ms = 0;
    _data.update_view = true;
    _data.channel_util = 0.0f;
    _data.window_ms = 0;
    _data.collisions = 0;
    _data.crc_errors = 0;
    _data.impersonations = 0;
}

void AppNodeRogue::onResume()
{
    ANIM_APP_OPEN();
    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas()->setFont(FONT_12);
    _data.hal->canvas()->setTextSize(1);
    _data.hal->canvas_update();

    _data.selected_index = 0;
    _data.scroll_offset = 0;
    _data.last_generation = 0xFFFFFFFF;
    _data.last_refresh_ms = 0;
    _data.update_view = true;
}

void AppNodeRogue::onRunning()
{
    uint32_t now = (uint32_t)millis();
    auto& log = Mesh::MeshDataStore::getInstance().getPacketLog();
    uint32_t gen = log.generation();

    if (gen != _data.last_generation || (now - _data.last_refresh_ms) >= REFRESH_INTERVAL_MS)
    {
        _data.last_generation = gen;
        _data.last_refresh_ms = now;
        _recompute();
        _data.update_view = true;
    }

    if (_data.update_view)
    {
        _render();
        _data.hal->canvas_update();
        _data.update_view = false;
    }

    _handle_input();
}

void AppNodeRogue::onDestroy() {}

void AppNodeRogue::_recompute()
{
    auto& log = Mesh::MeshDataStore::getInstance().getPacketLog();
    _data.stats.clear();
    _data.collisions = 0;
    _data.crc_errors = 0;
    _data.window_ms = 0;
    _data.impersonations = 0;

    auto* mesh = _data.hal->mesh();
    uint32_t our = mesh ? mesh->getNodeId() : 0;

    // Per-node misbehavior tracking over the ring window.
    // Static to keep the ~1KB replay table off the app-task stack; reset per call.
    static Mesh::ReplayTracker<Mesh::REPLAY_TRACK_SLOTS> replay;
    replay = Mesh::ReplayTracker<Mesh::REPLAY_TRACK_SLOTS>{};
    uint32_t relay_by_byte[256] = {0};
    uint32_t total_relayed = 0;

    // Get our position for distance calc
    float our_lat = 0, our_lon = 0;
    bool our_pos_valid = false;
    Mesh::NodeInfo our_ni;
    if (mesh && mesh->getNode(our, our_ni) && our_ni.info.position.latitude_i != 0) {
        our_lat = our_ni.info.position.latitude_i * 1e-7f;
        our_lon = our_ni.info.position.longitude_i * 1e-7f;
        our_pos_valid = true;
    }

    // Resolve LoRa parameters for the airtime estimate.
    int sf = 11, crd = 5;
    float bw = 250.0f;
    if (mesh)
    {
        const auto& lc = mesh->getConfig().lora_config;
        if (lc.use_preset)
        {
            const Mesh::ModemPresetInfo* mp = Mesh::getModemPresetInfo((int)lc.modem_preset);
            if (mp) { sf = mp->sf; crd = mp->cr; bw = mp->bw; }
        }
        else
        {
            sf = (int)lc.spread_factor; crd = (int)lc.coding_rate; bw = (float)lc.bandwidth;
            if (bw <= 0.0f) bw = 250.0f;
        }
    }

    int total = (int)log.size();
    uint32_t first_ts = 0, last_ts = 0;
    uint32_t prev_rx_ts = 0, prev_rx_air = 0;

    for (int i = 0; i < total; i++)
    {
        const auto& p = log[i];
        if (i == 0) first_ts = p.timestamp_ms;
        last_ts = p.timestamp_ms;
        if (p.is_tx) continue;

        uint32_t air = lora_toa_ms(p.size, sf, crd, bw);
        if (prev_rx_ts && p.timestamp_ms >= prev_rx_ts && (p.timestamp_ms - prev_rx_ts) < prev_rx_air)
            _data.collisions++;
        prev_rx_ts = p.timestamp_ms;
        prev_rx_air = air;

        if (p.crc_error) { _data.crc_errors++; continue; }
        // Impersonation: an RX packet claiming our own node id. (Legitimate
        // rebroadcasts of our own traffic also carry from==us, so this is an
        // awareness counter rather than a hard verdict.)
        if (p.from == our) { _data.impersonations++; continue; }
        if (p.from == 0) continue;

        // Relay attribution (by relay low-byte) + replay/duplicate tracking.
        if (p.relay_node != 0) { relay_by_byte[p.relay_node]++; total_relayed++; }
        uint16_t reps = replay.observe(p.from, p.id);

        NodeStat* st = nullptr;
        for (auto& s : _data.stats) if (s.node_id == p.from) { st = &s; break; }
        if (!st)
        {
            float dist = -1.0f;
            Mesh::NodeInfo ni;
            if (our_pos_valid && mesh->getNode(p.from, ni) && ni.info.position.latitude_i != 0) {
                dist = haversine_m(our_lat, our_lon, ni.info.position.latitude_i * 1e-7f, ni.info.position.longitude_i * 1e-7f);
            }
            _data.stats.push_back({p.from, 0, 0, 0, 0, 0.0f, 0, dist, 0, 0, 0, 0});
            st = &_data.stats.back();
        }
        st->count++;
        st->airtime_ms += air;
        st->sum_bytes += p.size;
        if (p.hop_start > st->max_hop_start) st->max_hop_start = p.hop_start;
        if (p.want_ack && p.to == 0xFFFFFFFFu && st->ack_bcast < 0xFFFF) st->ack_bcast++;
        if (reps > st->dup_max) st->dup_max = reps;
        if (p.timestamp_ms >= st->last_ts_ms)
        {
            st->last_ts_ms = p.timestamp_ms;
            st->last_rssi = p.rssi;
            st->last_snr = p.snr;
        }
    }

    // Attribute relay-flood behavior to the dominant relayer (by relay low-byte).
    int dom_relay_byte = -1;
    uint32_t dom_relay_count = 0;
    for (int b = 1; b < 256; b++)
        if (relay_by_byte[b] > dom_relay_count) { dom_relay_count = relay_by_byte[b]; dom_relay_byte = b; }
    bool relay_flood = total_relayed >= Mesh::THREAT_RELAY_MIN &&
                       dom_relay_count >= (uint32_t)(Mesh::THREAT_RELAY_SHARE * (float)total_relayed);

    // Finalize per-node threat flags.
    for (auto& s : _data.stats)
    {
        uint8_t f = Mesh::THREAT_NONE;
        if (s.max_hop_start > Mesh::THREAT_MAX_SANE_HOPS) f |= Mesh::THREAT_HOP_ABUSE;
        if (s.ack_bcast >= Mesh::THREAT_ACK_MIN) f |= Mesh::THREAT_ACK_AMP;
        if (s.dup_max > Mesh::THREAT_REPLAY_REPEATS) f |= Mesh::THREAT_REPLAY;
        if (relay_flood && dom_relay_byte >= 0 && (int)(s.node_id & 0xFF) == dom_relay_byte) f |= Mesh::THREAT_RELAY_FLOOD;
        s.threat_flags = f;
    }

    _data.window_ms = (last_ts >= first_ts) ? (last_ts - first_ts) : 0;

    if (_data.current_tab == Tab::TRAFFIC) {
        std::sort(_data.stats.begin(), _data.stats.end(),
                  [](const NodeStat& a, const NodeStat& b) { return a.airtime_ms > b.airtime_ms; });
    } else {
        std::sort(_data.stats.begin(), _data.stats.end(),
                  [](const NodeStat& a, const NodeStat& b) { return a.last_rssi > b.last_rssi; });
    }

    _data.channel_util = mesh ? mesh->getChannelUtilization() : 0.0f;
}

float AppNodeRogue::_airtime_share(const NodeStat& s) const
{
    float total = 0.0f;
    for (const auto& n : _data.stats) total += (float)n.airtime_ms;
    return total > 0.0f ? ((float)s.airtime_ms / total * 100.0f) : 0.0f;
}

float AppNodeRogue::_rate_ppm(const NodeStat& s) const
{
    float win_min = _data.window_ms / 60000.0f;
    return win_min > 0.001f ? ((float)s.count / win_min) : (float)s.count;
}

uint32_t AppNodeRogue::_row_color(const NodeStat& s, bool& is_rogue) const
{
    float share = _airtime_share(s);
    float rate = _rate_ppm(s);
    is_rogue = (share >= ROGUE_SHARE_PCT) || (rate >= ROGUE_RATE_PPM);
    if (is_rogue) return THEME_COLOR_SIGNAL_BAD;
    if (share >= WARN_SHARE_PCT || rate >= WARN_RATE_PPM) return THEME_COLOR_SIGNAL_FAIR;
    return THEME_COLOR_SIGNAL_GOOD;
}

uint32_t AppNodeRogue::_signal_color(const NodeStat& s, bool& is_anomaly) const
{
    // Heuristics for "too powerful" or "too weak"
    bool too_powerful = (s.last_rssi > -40);
    if (s.distance_m > 500.0f && s.last_rssi > -55) too_powerful = true;

    bool too_weak = (s.last_snr < -15.0f);
    if (s.distance_m > 0 && s.distance_m < 2000.0f && s.last_rssi < -110) too_weak = true;

    is_anomaly = too_powerful || too_weak;

    if (too_powerful) return THEME_COLOR_SIGNAL_BAD; // Red for "suspiciously powerful"
    if (too_weak) return THEME_COLOR_SIGNAL_FAIR;    // Yellow for "struggling"

    if (s.last_rssi > -90 && s.last_snr > -5.0f) return THEME_COLOR_SIGNAL_GOOD;
    if (s.last_rssi > -105 && s.last_snr > -12.0f) return THEME_COLOR_SIGNAL_FAIR;
    return THEME_COLOR_SIGNAL_BAD;
}

// Build a compact distinct-letter string for the set threat flags.
// I=impersonation, H=hop abuse, R=relay flood, A=ack amplifier, D=replay/dup.
int AppNodeRogue::_threat_str(uint8_t flags, char* out, size_t cap)
{
    int n = 0;
    auto add = [&](char c) { if ((size_t)n + 1 < cap) out[n++] = c; };
    if (flags & Mesh::THREAT_IMPERSONATE) add('I');
    if (flags & Mesh::THREAT_HOP_ABUSE)   add('H');
    if (flags & Mesh::THREAT_RELAY_FLOOD) add('R');
    if (flags & Mesh::THREAT_ACK_AMP)     add('A');
    if (flags & Mesh::THREAT_REPLAY)      add('D');
    out[n] = '\0';
    return n;
}

std::string AppNodeRogue::_node_label(uint32_t node_id) const
{
    auto* ndb = _data.hal->nodedb();
    if (ndb)
    {
        const Mesh::NodeIndexEntry* e = ndb->getNodeIndex(node_id);
        if (e) return Mesh::NodeDB::getIndexLabel(*e);
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "!%04X", (unsigned)(node_id & 0xFFFF));
    return std::string(buf);
}

void AppNodeRogue::_render()
{
    auto* canvas = _data.hal->canvas();
    canvas->fillScreen(THEME_COLOR_BG);

    _render_tabs();

    if (_data.current_tab == Tab::HISTORY)
    {
        _render_history_tab();
        return;
    }

    if (_data.stats.empty())
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("<no traffic>", canvas->width() / 2, canvas->height() / 2);
        return;
    }

    if (_data.current_tab == Tab::TRAFFIC) _render_traffic_tab();
    else _render_signal_tab();
}

void AppNodeRogue::_render_tabs()
{
    auto* canvas = _data.hal->canvas();
    int w = canvas->width() / 3;

    struct { Tab tab; const char* name; } tabs[3] = {
        {Tab::TRAFFIC, "TRAFFIC"}, {Tab::SIGNAL, "SIGNAL"}, {Tab::HISTORY, "HIST"}};
    for (int i = 0; i < 3; i++)
    {
        int x = i * w;
        int tw = (i == 2) ? (canvas->width() - x) : w; // last tab absorbs rounding
        bool sel = _data.current_tab == tabs[i].tab;
        canvas->fillRect(x, 0, tw, TAB_HEIGHT, sel ? THEME_COLOR_BG_SELECTED : THEME_COLOR_BG_DARK);
        canvas->setTextColor(sel ? TFT_WHITE : TFT_DARKGREY);
        canvas->setFont(FONT_10);
        canvas->drawCenterString(tabs[i].name, x + tw / 2, 3);
    }

    canvas->drawFastHLine(0, TAB_HEIGHT, canvas->width(), THEME_COLOR_HEADER_LINE);

    // Sub-header
    int total = (int)_data.stats.size();
    canvas->setFont(FONT_10);
    canvas->setTextColor(TFT_ORANGE);
    char hdr[40];
    if (_data.current_tab == Tab::TRAFFIC)
        snprintf(hdr, sizeof(hdr), "UTIL:%d%% COL:%lu IMP:%lu", (int)(_data.channel_util + 0.5f),
                 (unsigned long)_data.collisions, (unsigned long)_data.impersonations);
    else if (_data.current_tab == Tab::SIGNAL)
        snprintf(hdr, sizeof(hdr), "NODES: %d", total);
    else
    {
        auto& ds = Mesh::MeshDataStore::getInstance();
        snprintf(hdr, sizeof(hdr), "%luh PKTS:%lu IMP:%lu", (unsigned long)(ds.getThreatWindowSeconds() / 3600),
                 (unsigned long)ds.getThreatTotalPackets(), (unsigned long)ds.getThreatImpersonations());
    }
    canvas->drawString(hdr, 4, TAB_HEIGHT + 1);

    canvas->drawFastHLine(0, HEADER_HEIGHT - 1, canvas->width(), THEME_COLOR_HEADER_LINE);
    canvas->setFont(FONT_12);
}

void AppNodeRogue::_render_traffic_tab()
{
    auto* canvas = _data.hal->canvas();
    int total = (int)_data.stats.size();
    int list_h = canvas->height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    int max_rows = std::max(1, list_h / ROW_HEIGHT);

    if (_data.selected_index >= total) _data.selected_index = total - 1;
    if (_data.selected_index < 0) _data.selected_index = 0;
    if (_data.selected_index < _data.scroll_offset) _data.scroll_offset = _data.selected_index;
    if (_data.selected_index >= _data.scroll_offset + max_rows) _data.scroll_offset = _data.selected_index - max_rows + 1;

    int bar_x = 168;
    int bar_w = canvas->width() - bar_x - 4;

    for (int i = 0; i < max_rows && (_data.scroll_offset + i) < total; i++)
    {
        int idx = _data.scroll_offset + i;
        const NodeStat& s = _data.stats[idx];
        int y = HEADER_HEIGHT + i * ROW_HEIGHT;
        bool selected = (idx == _data.selected_index);

        if (selected) canvas->fillRect(1, y, canvas->width() - 2, ROW_HEIGHT, THEME_COLOR_BG_SELECTED);

        bool is_rogue = false;
        uint32_t color = _row_color(s, is_rogue);
        float share = _airtime_share(s);

        if (is_rogue) { canvas->setTextColor(TFT_RED); canvas->drawString("!", 2, y + 1); }

        canvas->setTextColor(selected ? TFT_WHITE : color);
        canvas->drawString(_node_label(s.node_id).c_str(), 10, y + 1);

        // Distinct per-node threat flags (I/H/R/A/D), right-aligned before stats.
        char tf[8];
        if (_threat_str(s.threat_flags, tf, sizeof(tf)) > 0)
        {
            canvas->setFont(FONT_10);
            canvas->setTextColor(TFT_RED);
            canvas->drawRightString(tf, 94, y + 2);
        }

        canvas->setFont(FONT_10);
        char mid[24];
        snprintf(mid, sizeof(mid), "%lup %d%%", (unsigned long)s.count, (int)(share + 0.5f));
        canvas->setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY);
        canvas->drawString(mid, 96, y + 2);

        canvas->drawRect(bar_x, y + 2, bar_w, ROW_HEIGHT - 5, THEME_COLOR_HEADER_LINE);
        int fill = (int)((share / 100.0f) * (bar_w - 2));
        if (fill > 0) canvas->fillRect(bar_x + 1, y + 3, std::min(fill, bar_w - 2), ROW_HEIGHT - 7, color);
        canvas->setFont(FONT_12);
    }

    // Footer
    int foot_y = canvas->height() - FOOTER_HEIGHT;
    canvas->drawFastHLine(0, foot_y, canvas->width(), THEME_COLOR_HEADER_LINE);
    const NodeStat& sel = _data.stats[_data.selected_index];
    canvas->setFont(FONT_10);
    char det[64];
    char tf[8];
    _threat_str(sel.threat_flags, tf, sizeof(tf));
    snprintf(det, sizeof(det), "%.1fp/m %.0fB %.1fdB%s%s", _rate_ppm(sel), (double)sel.sum_bytes,
             (double)sel.last_snr, tf[0] ? " " : "", tf);
    canvas->setTextColor(sel.threat_flags ? TFT_RED : TFT_CYAN);
    canvas->drawString(det, 2, foot_y + 2);
    char air[24];
    snprintf(air, sizeof(air), "air:%.1fs", sel.airtime_ms / 1000.0f);
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawRightString(air, canvas->width() - 2, foot_y + 2);
    canvas->drawCenterString(HINT, canvas->width() / 2, canvas->height() - 11);
    canvas->setFont(FONT_12);
}

void AppNodeRogue::_render_signal_tab()
{
    auto* canvas = _data.hal->canvas();
    int total = (int)_data.stats.size();
    int list_h = canvas->height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    int max_rows = std::max(1, list_h / ROW_HEIGHT);

    if (_data.selected_index >= total) _data.selected_index = total - 1;
    if (_data.selected_index < 0) _data.selected_index = 0;
    if (_data.selected_index < _data.scroll_offset) _data.scroll_offset = _data.selected_index;
    if (_data.selected_index >= _data.scroll_offset + max_rows) _data.scroll_offset = _data.selected_index - max_rows + 1;

    for (int i = 0; i < max_rows && (_data.scroll_offset + i) < total; i++)
    {
        int idx = _data.scroll_offset + i;
        const NodeStat& s = _data.stats[idx];
        int y = HEADER_HEIGHT + i * ROW_HEIGHT;
        bool selected = (idx == _data.selected_index);

        if (selected) canvas->fillRect(1, y, canvas->width() - 2, ROW_HEIGHT, THEME_COLOR_BG_SELECTED);

        bool is_anomaly = false;
        uint32_t color = _signal_color(s, is_anomaly);

        if (is_anomaly) { canvas->setTextColor(TFT_YELLOW); canvas->drawString("?", 2, y + 1); }

        canvas->setTextColor(selected ? TFT_WHITE : color);
        canvas->drawString(_node_label(s.node_id).c_str(), 10, y + 1);

        char sig[32];
        snprintf(sig, sizeof(sig), "%4d dBm  %5.1f dB", s.last_rssi, (double)s.last_snr);
        canvas->setFont(FONT_10);
        canvas->setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY);
        canvas->drawString(sig, 100, y + 2);
        canvas->setFont(FONT_12);
    }

    // Footer
    int foot_y = canvas->height() - FOOTER_HEIGHT;
    canvas->drawFastHLine(0, foot_y, canvas->width(), THEME_COLOR_HEADER_LINE);
    const NodeStat& sel = _data.stats[_data.selected_index];
    canvas->setFont(FONT_10);
    char det[64];

    bool is_anomaly = false;
    _signal_color(sel, is_anomaly);

    const char* diag = "";
    if (is_anomaly) {
        if (sel.last_rssi > -45) diag = " [TOO POWERFUL?]";
        else diag = " [WEAK/NOISY]";
    }

    if (sel.distance_m >= 0) {
        if (sel.distance_m < 1000) snprintf(det, sizeof(det), "%.0fm%s", sel.distance_m, diag);
        else snprintf(det, sizeof(det), "%.2fkm%s", sel.distance_m / 1000.0f, diag);
    } else {
        snprintf(det, sizeof(det), "Dist: unknown%s", diag);
    }
    canvas->setTextColor(is_anomaly ? TFT_YELLOW : TFT_CYAN);
    canvas->drawString(det, 2, foot_y + 2);

    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawCenterString(HINT, canvas->width() / 2, canvas->height() - 11);
    canvas->setFont(FONT_12);
}

void AppNodeRogue::_render_history_tab()
{
    auto* canvas = _data.hal->canvas();
    auto& ds = Mesh::MeshDataStore::getInstance();

    if (!ds.hasThreatSummary())
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("<no history at boot>", canvas->width() / 2, canvas->height() / 2);
        return;
    }

    const Mesh::ThreatOffender* off = ds.getThreatOffenders();
    int total = ds.getThreatOffenderCount();
    if (total == 0)
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("<no offenders>", canvas->width() / 2, canvas->height() / 2);
        return;
    }

    int list_h = canvas->height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    int max_rows = std::max(1, list_h / ROW_HEIGHT);

    if (_data.selected_index >= total) _data.selected_index = total - 1;
    if (_data.selected_index < 0) _data.selected_index = 0;
    if (_data.selected_index < _data.scroll_offset) _data.scroll_offset = _data.selected_index;
    if (_data.selected_index >= _data.scroll_offset + max_rows) _data.scroll_offset = _data.selected_index - max_rows + 1;

    for (int i = 0; i < max_rows && (_data.scroll_offset + i) < total; i++)
    {
        int idx = _data.scroll_offset + i;
        const Mesh::ThreatOffender& o = off[idx];
        int y = HEADER_HEIGHT + i * ROW_HEIGHT;
        bool selected = (idx == _data.selected_index);

        if (selected) canvas->fillRect(1, y, canvas->width() - 2, ROW_HEIGHT, THEME_COLOR_BG_SELECTED);

        if (o.flags) { canvas->setTextColor(TFT_RED); canvas->drawString("!", 2, y + 1); }

        if (selected) canvas->setTextColor(TFT_WHITE);
        else if (o.flags) canvas->setTextColor(TFT_RED);
        else canvas->setTextColor(THEME_COLOR_SIGNAL_GOOD);
        canvas->drawString(_node_label(o.node_id).c_str(), 10, y + 1);

        char tf[8];
        _threat_str(o.flags, tf, sizeof(tf));
        canvas->setFont(FONT_10);
        if (tf[0])
        {
            canvas->setTextColor(TFT_RED);
            canvas->drawRightString(tf, 94, y + 2);
        }
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%lup", (unsigned long)o.count);
        canvas->setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY);
        canvas->drawString(cnt, 100, y + 2);
        canvas->setFont(FONT_12);
    }

    // Footer: breakdown for the selected offender.
    int foot_y = canvas->height() - FOOTER_HEIGHT;
    canvas->drawFastHLine(0, foot_y, canvas->width(), THEME_COLOR_HEADER_LINE);
    const Mesh::ThreatOffender& sel = off[_data.selected_index];
    canvas->setFont(FONT_10);
    char det[64];
    snprintf(det, sizeof(det), "hop:%u ackB:%u dup:%u", (unsigned)sel.max_hop_start, (unsigned)sel.ack_bcast,
             (unsigned)sel.dup_max);
    canvas->setTextColor(sel.flags ? TFT_RED : TFT_CYAN);
    canvas->drawString(det, 2, foot_y + 2);
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawCenterString(HINT, canvas->width() / 2, canvas->height() - 11);
    canvas->setFont(FONT_12);
}

void AppNodeRogue::_handle_input()
{
    static bool is_repeat = false;
    static uint32_t next_fire_ts = 0;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (!_data.hal->keyboard()->isPressed()) { is_repeat = false; return; }

    uint32_t now = (uint32_t)millis();

    if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE) ||
        _data.hal->home_button()->is_pressed())
    {
        _data.hal->playNextSound();
        _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
        destroyApp();
        return;
    }

    // Number of rows in the currently active list (differs for HISTORY).
    int list_count = (_data.current_tab == Tab::HISTORY)
                         ? Mesh::MeshDataStore::getInstance().getThreatOffenderCount()
                         : (int)_data.stats.size();

    if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.current_tab != Tab::HISTORY)
        {
            _data.current_tab = (_data.current_tab == Tab::TRAFFIC) ? Tab::SIGNAL : Tab::HISTORY;
            _data.selected_index = 0; _data.scroll_offset = 0;
            _recompute(); _data.hal->playNextSound(); _data.update_view = true;
        }
    }
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.current_tab != Tab::TRAFFIC)
        {
            _data.current_tab = (_data.current_tab == Tab::HISTORY) ? Tab::SIGNAL : Tab::TRAFFIC;
            _data.selected_index = 0; _data.scroll_offset = 0;
            _recompute(); _data.hal->playNextSound(); _data.update_view = true;
        }
    }
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index < list_count - 1)
        {
            _data.selected_index++; _data.hal->playNextSound(); _data.update_view = true;
        }
    }
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index > 0)
        {
            _data.selected_index--; _data.hal->playNextSound(); _data.update_view = true;
        }
    }
}
