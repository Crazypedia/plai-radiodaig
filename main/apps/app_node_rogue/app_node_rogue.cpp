/**
 * @file app_node_rogue.cpp
 * @brief Rogue Node Tracker - flags noisy nodes by airtime/rate + collision warnings
 * @version 1.0
 * @date 2026-06-27
 */
#include "app_node_rogue.h"
#include "common_define.h"
#include "apps/utils/theme/theme_define.h"
#include "apps/utils/ui/key_repeat.h"
#include "mesh/mesh_service.h"
#include "mesh/mesh_data.h"
#include "mesh/node_db.h"
#include <algorithm>
#include <math.h>

using namespace MOONCAKE::APPS;

#define HEADER_HEIGHT 11
#define FOOTER_HEIGHT 38
#define ROW_HEIGHT 15

// Heuristic thresholds for flagging a node as noisy/rogue.
#define ROGUE_SHARE_PCT 35.0f // % of observed channel airtime
#define WARN_SHARE_PCT 20.0f
#define ROGUE_RATE_PPM 15.0f // packets per minute from one node
#define WARN_RATE_PPM 8.0f

#define REFRESH_INTERVAL_MS 1000

// Over-powered-antenna detection.
//
// We can't measure a remote node's TX power directly, but we can bound it. For a
// given distance the received power can't exceed (EIRP - free-space path loss),
// since free-space loss (path-loss exponent 2) is the *least* loss physically
// possible. So a lower bound on the transmitter's EIRP is:
//
//     EIRP_min(dBm) = measured_RSSI(dBm) + FSPL(distance, frequency)
//
// (assuming ~0 dBi at our receiver). Real-world loss is higher than free space, so
// the true EIRP is >= EIRP_min. If even this lower bound is large, the node is
// transmitting harder than typical mesh hardware/regs allow (handhelds are usually
// ~+20..+27 dBm EIRP; +30 dBm = 1 W, +33 dBm = 2 W).
#define ROGUE_EIRP_DBM 33.0f    // EIRP_min at/above this => over-powered (>= ~2 W)
#define WARN_EIRP_DBM 30.0f     // borderline (>= ~1 W)
#define EIRP_MIN_DIST_KM 0.10f  // ignore closer than this (near-field/GPS noise)
#define EIRP_MIN_PACKETS 2u     // need a couple of receptions to trust the RSSI

static const char* HINT = "[↑][↓] [ESC]";

// Great-circle distance in km between two lat/lon in degrees.
static double haversine_km(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) * sin(dlon / 2) * sin(dlon / 2);
    return 2.0 * R * atan2(sqrt(a), sqrt(1.0 - a));
}

// Free-space path loss in dB. d in km, f in MHz.
static double fspl_db(double d_km, double f_mhz)
{
    if (d_km <= 0.0 || f_mhz <= 0.0)
        return 0.0;
    return 32.44 + 20.0 * log10(d_km) + 20.0 * log10(f_mhz);
}

// LoRa time-on-air (ms) for an explicit-header, CRC-on packet.
// cr_denom is the 4/N denominator (5..8); bw in kHz.
static uint32_t lora_toa_ms(uint16_t payload_len, int sf, int cr_denom, float bw_khz)
{
    if (sf < 6)
        sf = 6;
    if (sf > 12)
        sf = 12;
    double bw = (double)bw_khz * 1000.0;
    if (bw <= 0.0)
        bw = 250000.0;
    int cr = cr_denom;
    if (cr < 5)
        cr += 4; // normalize 1..4 -> 5..8 just in case
    if (cr > 8)
        cr = 8;

    double tsym = (double)(1u << sf) / bw; // seconds per symbol
    int de = (tsym > 0.016) ? 1 : 0;       // low-data-rate optimize
    const int preamble = 16, crc = 1, ih = 0;

    double t_preamble = (preamble + 4.25) * tsym;
    double num = 8.0 * payload_len - 4.0 * sf + 28.0 + 16.0 * crc - 20.0 * ih;
    double den = 4.0 * (sf - 2 * de);
    double payload_symb = 8.0;
    if (num > 0.0 && den > 0.0)
        payload_symb += ceil(num / den) * (double)cr; // cr == CR+4 == 4/N denominator

    double toa = t_preamble + payload_symb * tsym;
    return (uint32_t)(toa * 1000.0 + 0.5);
}

void AppNodeRogue::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.selected_index = 0;
    _data.scroll_offset = 0;
    _data.last_generation = 0xFFFFFFFF;
    _data.last_refresh_ms = 0;
    _data.update_view = true;
    _data.channel_util = 0.0f;
    _data.window_ms = 0;
    _data.collisions = 0;
    _data.crc_errors = 0;
    _data.our_lat_i = 0;
    _data.our_lon_i = 0;
    _data.our_pos_valid = false;
    _data.frequency_mhz = 0.0f;
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

    auto* mesh = _data.hal->mesh();
    uint32_t our = mesh ? mesh->getNodeId() : 0;

    // Resolve LoRa parameters for the airtime estimate.
    int sf = 11, crd = 5;
    float bw = 250.0f;
    if (mesh)
    {
        const auto& lc = mesh->getConfig().lora_config;
        if (lc.use_preset)
        {
            const Mesh::ModemPresetInfo* mp = Mesh::getModemPresetInfo((int)lc.modem_preset);
            if (mp)
            {
                sf = mp->sf;
                crd = mp->cr;
                bw = mp->bw;
            }
        }
        else
        {
            sf = (int)lc.spread_factor;
            crd = (int)lc.coding_rate;
            bw = (float)lc.bandwidth;
            if (bw <= 0.0f)
                bw = 250.0f;
        }
    }

    int total = (int)log.size();
    uint32_t first_ts = 0, last_ts = 0;
    uint32_t prev_rx_ts = 0, prev_rx_air = 0;

    for (int i = 0; i < total; i++)
    {
        const auto& p = log[i]; // chronological: [0]=oldest
        if (i == 0)
            first_ts = p.timestamp_ms;
        last_ts = p.timestamp_ms;

        if (p.is_tx)
            continue; // only count what we heard from the air

        uint32_t air = lora_toa_ms(p.size, sf, crd, bw);

        // Overlapping transmissions => potential collision at our receiver.
        if (prev_rx_ts && p.timestamp_ms >= prev_rx_ts && (p.timestamp_ms - prev_rx_ts) < prev_rx_air)
            _data.collisions++;
        prev_rx_ts = p.timestamp_ms;
        prev_rx_air = air;

        if (p.crc_error)
        {
            _data.crc_errors++;
            continue; // from/to may be garbage on CRC failure
        }
        if (p.from == our || p.from == 0)
            continue;

        NodeStat* st = nullptr;
        for (auto& s : _data.stats)
            if (s.node_id == p.from)
            {
                st = &s;
                break;
            }
        if (!st)
        {
            _data.stats.push_back({p.from, 0, 0, 0, 0.0f, 0});
            st = &_data.stats.back();
        }
        st->count++;
        st->airtime_ms += air;
        st->sum_bytes += p.size;
        if (p.timestamp_ms >= st->last_ts_ms)
        {
            st->last_ts_ms = p.timestamp_ms;
            st->last_snr = p.snr;
        }
    }

    _data.window_ms = (last_ts >= first_ts) ? (last_ts - first_ts) : 0;
    std::sort(_data.stats.begin(), _data.stats.end(),
              [](const NodeStat& a, const NodeStat& b) { return a.airtime_ms > b.airtime_ms; });

    _data.channel_util = mesh ? mesh->getChannelUtilization() : 0.0f;

    // --- Over-power estimate inputs: our position + radio frequency ---
    _data.frequency_mhz = mesh ? mesh->getFrequency() : 0.0f;
    if (_data.frequency_mhz <= 0.0f)
        _data.frequency_mhz = 915.0f; // sane default if unconfigured

    _data.our_pos_valid = false;
    if (mesh)
    {
        const auto& cfg = mesh->getConfig();
        if (cfg.position == Mesh::MeshConfig::POSITION_FIXED &&
            (cfg.fixed_latitude != 0 || cfg.fixed_longitude != 0))
        {
            _data.our_lat_i = cfg.fixed_latitude;
            _data.our_lon_i = cfg.fixed_longitude;
            _data.our_pos_valid = true;
        }
#if HAL_USE_GPS
        else if (cfg.position == Mesh::MeshConfig::POSITION_GPS)
        {
            auto* gps = _data.hal->gps();
            if (gps && gps->hasFix())
            {
                _data.our_lat_i = gps->getLatitudeI();
                _data.our_lon_i = gps->getLongitudeI();
                _data.our_pos_valid = true;
            }
        }
#endif
    }

    // Pull last RSSI from the node db and estimate a lower-bound EIRP per node.
    auto* ndb = _data.hal->nodedb();
    for (auto& s : _data.stats)
    {
        if (ndb)
        {
            const Mesh::NodeIndexEntry* e = ndb->getNodeIndex(s.node_id);
            if (e)
                s.last_rssi = e->last_rssi;
        }
        _estimate_eirp(s);
    }
}

// Estimate a lower bound on the node's EIRP from its received signal strength and
// great-circle distance (see threshold notes at the top of this file). Only valid
// when both our node and the remote node have a usable position and the node is far
// enough away that the path-loss model is meaningful.
void AppNodeRogue::_estimate_eirp(NodeStat& s) const
{
    s.has_eirp = false;
    s.eirp_min_dbm = 0.0f;
    s.dist_km = 0.0f;

    if (!_data.our_pos_valid || s.count < EIRP_MIN_PACKETS || s.last_rssi == 0)
        return;

    auto* ndb = _data.hal->nodedb();
    if (!ndb)
        return;
    const Mesh::NodeIndexEntry* e = ndb->getNodeIndex(s.node_id);
    if (!e || (e->latitude_i == 0 && e->longitude_i == 0))
        return; // remote node has no position

    double our_lat = _data.our_lat_i * 1e-7, our_lon = _data.our_lon_i * 1e-7;
    double their_lat = e->latitude_i * 1e-7, their_lon = e->longitude_i * 1e-7;
    double d_km = haversine_km(our_lat, our_lon, their_lat, their_lon);
    if (d_km < EIRP_MIN_DIST_KM)
        return; // too close to trust (near-field / GPS noise)

    s.dist_km = (float)d_km;
    s.eirp_min_dbm = (float)((double)s.last_rssi + fspl_db(d_km, _data.frequency_mhz));
    s.has_eirp = true;
}

bool AppNodeRogue::_is_overpowered(const NodeStat& s) const
{
    return s.has_eirp && s.eirp_min_dbm >= WARN_EIRP_DBM;
}

float AppNodeRogue::_airtime_share(const NodeStat& s) const
{
    float total = 0.0f;
    for (const auto& n : _data.stats)
        total += (float)n.airtime_ms;
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
    if (is_rogue)
        return THEME_COLOR_SIGNAL_BAD;
    if (share >= WARN_SHARE_PCT || rate >= WARN_RATE_PPM)
        return THEME_COLOR_SIGNAL_FAIR;
    return THEME_COLOR_SIGNAL_GOOD;
}

std::string AppNodeRogue::_node_label(uint32_t node_id) const
{
    auto* ndb = _data.hal->nodedb();
    if (ndb)
    {
        const Mesh::NodeIndexEntry* e = ndb->getNodeIndex(node_id);
        if (e)
            return Mesh::NodeDB::getIndexLabel(*e);
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "!%04X", (unsigned)(node_id & 0xFFFF));
    return std::string(buf);
}

void AppNodeRogue::_render()
{
    auto* canvas = _data.hal->canvas();
    canvas->fillScreen(THEME_COLOR_BG);
    canvas->setFont(FONT_12);

    int total = (int)_data.stats.size();

    // Header: title + channel util + collision count
    canvas->setTextColor(TFT_ORANGE);
    char hdr[20];
    snprintf(hdr, sizeof(hdr), "ROGUE  %dn", total);
    canvas->drawString(hdr, 2, 0);

    canvas->setFont(FONT_10);
    char rhdr[24];
    snprintf(rhdr, sizeof(rhdr), "ch:%d%%  %luX", (int)(_data.channel_util + 0.5f),
             (unsigned long)_data.collisions);
    canvas->setTextColor(_data.collisions > 0 ? TFT_RED : TFT_DARKGREY);
    canvas->drawRightString(rhdr, canvas->width() - 2, 1);
    canvas->setFont(FONT_12);
    canvas->drawFastHLine(0, HEADER_HEIGHT - 1, canvas->width(), THEME_COLOR_HEADER_LINE);

    if (total == 0)
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("<no traffic>", canvas->width() / 2, canvas->height() / 2 - 6);
        return;
    }

    if (_data.selected_index >= total)
        _data.selected_index = total - 1;
    if (_data.selected_index < 0)
        _data.selected_index = 0;

    int list_h = canvas->height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    int max_rows = std::max(1, list_h / ROW_HEIGHT);

    if (_data.selected_index < _data.scroll_offset)
        _data.scroll_offset = _data.selected_index;
    if (_data.selected_index >= _data.scroll_offset + max_rows)
        _data.scroll_offset = _data.selected_index - max_rows + 1;
    if (_data.scroll_offset < 0)
        _data.scroll_offset = 0;

    int bar_x = 168;
    int bar_w = canvas->width() - bar_x - 4;

    for (int i = 0; i < max_rows && (_data.scroll_offset + i) < total; i++)
    {
        int idx = _data.scroll_offset + i;
        const NodeStat& s = _data.stats[idx];
        int y = HEADER_HEIGHT + i * ROW_HEIGHT;
        bool selected = (idx == _data.selected_index);

        if (selected)
            canvas->fillRect(1, y, canvas->width() - 2, ROW_HEIGHT, THEME_COLOR_BG_SELECTED);

        bool is_rogue = false;
        uint32_t color = _row_color(s, is_rogue);
        bool overpowered = _is_overpowered(s);
        if (overpowered)
            color = TFT_MAGENTA; // over-power dominates the row's color
        float share = _airtime_share(s);

        // Marker: over-power ("^") takes precedence over channel-hog ("!")
        if (overpowered)
        {
            canvas->setTextColor(TFT_MAGENTA);
            canvas->drawString("^", 2, y + 1);
        }
        else if (is_rogue)
        {
            canvas->setTextColor(TFT_RED);
            canvas->drawString("!", 2, y + 1);
        }

        // Node label
        canvas->setFont(FONT_12);
        canvas->setTextColor(selected ? TFT_WHITE : color);
        std::string label = _node_label(s.node_id);
        canvas->drawString(label.c_str(), 10, y + 1);

        // Packet count + share %
        canvas->setFont(FONT_10);
        char mid[20];
        snprintf(mid, sizeof(mid), "%lup %d%%", (unsigned long)s.count, (int)(share + 0.5f));
        canvas->setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY);
        canvas->drawString(mid, 96, y + 2);

        // Airtime-share bar
        canvas->drawRect(bar_x, y + 2, bar_w, ROW_HEIGHT - 5, THEME_COLOR_HEADER_LINE);
        int fill = (int)((share / 100.0f) * (bar_w - 2));
        if (fill < 0)
            fill = 0;
        if (fill > bar_w - 2)
            fill = bar_w - 2;
        if (fill > 0)
            canvas->fillRect(bar_x + 1, y + 3, fill, ROW_HEIGHT - 7, color);
    }
    canvas->setFont(FONT_12);

    // Footer: selected node detail + hint
    int foot_y = canvas->height() - FOOTER_HEIGHT;
    canvas->drawFastHLine(0, foot_y, canvas->width(), THEME_COLOR_HEADER_LINE);

    const NodeStat& sel = _data.stats[_data.selected_index];
    canvas->setFont(FONT_10);
    char det[40];
    snprintf(det, sizeof(det), "%.1f p/m  %.0fB  %.1fdB", _rate_ppm(sel), (double)sel.sum_bytes,
             (double)sel.last_snr);
    canvas->setTextColor(TFT_CYAN);
    canvas->drawString(det, 2, foot_y + 2);

    char air[24];
    snprintf(air, sizeof(air), "air:%.1fs", _data.stats[_data.selected_index].airtime_ms / 1000.0f);
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawRightString(air, canvas->width() - 2, foot_y + 2);

    // Over-power line for the selected node: estimated lower-bound EIRP + distance.
    if (sel.has_eirp)
    {
        char ep[40];
        snprintf(ep, sizeof(ep), "EIRP>=%ddBm @%.1fkm%s", (int)(sel.eirp_min_dbm + 0.5f), (double)sel.dist_km,
                 sel.eirp_min_dbm >= ROGUE_EIRP_DBM ? "  HOT" : "");
        canvas->setTextColor(_is_overpowered(sel) ? TFT_MAGENTA : TFT_DARKGREY);
        canvas->drawString(ep, 2, foot_y + 13);
    }
    else
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawString(_data.our_pos_valid ? "EIRP: node has no position" : "EIRP: set our position (GPS/fixed)",
                           2, foot_y + 13);
    }

    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawCenterString(HINT, canvas->width() / 2, canvas->height() - 11);
    canvas->setFont(FONT_12);
}

void AppNodeRogue::_handle_input()
{
    static bool is_repeat = false;
    static uint32_t next_fire_ts = 0;

    int total = (int)_data.stats.size();

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (!_data.hal->keyboard()->isPressed())
    {
        is_repeat = false;
        return;
    }

    uint32_t now = (uint32_t)millis();

    if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE) ||
        _data.hal->home_button()->is_pressed())
    {
        _data.hal->playNextSound();
        _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
        destroyApp();
        return;
    }
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index < total - 1)
        {
            _data.selected_index++;
            _data.hal->playNextSound();
            _data.update_view = true;
        }
    }
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index > 0)
        {
            _data.selected_index--;
            _data.hal->playNextSound();
            _data.update_view = true;
        }
    }
}
