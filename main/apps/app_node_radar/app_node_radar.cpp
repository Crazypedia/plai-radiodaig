/**
 * @file app_node_radar.cpp
 * @brief Node Radar - geographic bearing arrow + live RSSI/SNR signal homing.
 * @version 1.0
 * @date 2026-06-27
 */
#include "app_node_radar.h"
#include "common_define.h"
#include "apps/utils/theme/theme_define.h"
#include "apps/utils/ui/key_repeat.h"
#include "mesh/node_db.h"
#include <algorithm>
#include <math.h>
#include <ctype.h>
#include <time.h>

using namespace MOONCAKE::APPS;

#define HEADER_HEIGHT 11
#define FOOTER_HEIGHT 11
#define ROW_HEIGHT 14
#define RADAR_REFRESH_MS 250
#define FILTER_MAX 12

// RSSI window for the signal meter (dBm).
#define RSSI_MIN -120.0f
#define RSSI_MAX -30.0f

static const char* SEL_HINT = "type=filter [↑↓][ENTER][ESC]";

static float deg2rad(float d) { return d * (float)M_PI / 180.0f; }

// Great-circle distance in meters between two lat/lon (degrees).
static double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) * sin(dlon / 2) * sin(dlon / 2);
    return 2.0 * R * atan2(sqrt(a), sqrt(1.0 - a));
}

// Initial bearing (degrees, 0=N) from point 1 to point 2.
static float bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) * 180.0 / M_PI;
    return (float)(b < 0 ? b + 360.0 : b);
}

void AppNodeRadar::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.view = View::SELECT;
    _data.filter.clear();
    _data.filtered.clear();
    _data.selected_index = 0;
    _data.scroll_offset = 0;
    _data.last_change_counter = 0xFFFFFFFF;
    _data.target_id = 0;
    _data.rssi_hist_count = 0;
    _data.prev_rssi = 0;
    _data.last_refresh_ms = 0;
    _data.update_view = true;
}

void AppNodeRadar::onResume()
{
    ANIM_APP_OPEN();
    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas()->setFont(FONT_12);
    _data.hal->canvas()->setTextSize(1);
    _data.hal->canvas_update();

    _data.view = View::SELECT;
    _data.last_change_counter = 0xFFFFFFFF;
    _data.update_view = true;
}

void AppNodeRadar::onRunning()
{
    uint32_t now = (uint32_t)millis();

    if (_data.view == View::SELECT)
    {
        auto* nodedb = _data.hal->nodedb();
        if (nodedb)
        {
            uint32_t cc = nodedb->getChangeCounter();
            if (cc != _data.last_change_counter)
            {
                _data.last_change_counter = cc;
                _rebuild_filtered();
                _data.update_view = true;
            }
        }
        if (_data.update_view)
        {
            _render_select();
            _data.hal->canvas_update();
            _data.update_view = false;
        }
        _handle_select_input();
    }
    else
    {
        if ((now - _data.last_refresh_ms) >= RADAR_REFRESH_MS)
        {
            _data.last_refresh_ms = now;
            Mesh::NodeIndexEntry e;
            if (_lookup_target(e))
                _push_rssi(e.last_rssi);
            _data.update_view = true;
        }
        if (_data.update_view)
        {
            _render_radar();
            _data.hal->canvas_update();
            _data.update_view = false;
        }
        _handle_radar_input();
    }
}

void AppNodeRadar::onDestroy() {}

std::string AppNodeRadar::_node_label(uint32_t node_id) const
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

// ---------------- SELECT view ----------------

void AppNodeRadar::_rebuild_filtered()
{
    auto* ndb = _data.hal->nodedb();
    _data.filtered.clear();
    if (!ndb)
        return;

    ndb->sortIndex(Mesh::SortOrder::LAST_HEARD);
    const auto& index = ndb->getIndex();

    for (const auto& e : index)
    {
        if (_data.filter.empty())
        {
            _data.filtered.push_back(e.node_id);
            continue;
        }
        char hay[32];
        snprintf(hay, sizeof(hay), "%s %s %08x", e.short_name, e.long_name, (unsigned)e.node_id);
        std::string h(hay);
        std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (h.find(_data.filter) != std::string::npos)
            _data.filtered.push_back(e.node_id);
    }

    if (_data.selected_index >= (int)_data.filtered.size())
        _data.selected_index = (int)_data.filtered.size() - 1;
    if (_data.selected_index < 0)
        _data.selected_index = 0;
}

void AppNodeRadar::_render_select()
{
    auto* canvas = _data.hal->canvas();
    auto* ndb = _data.hal->nodedb();
    canvas->fillScreen(THEME_COLOR_BG);
    canvas->setFont(FONT_12);

    int total = (int)_data.filtered.size();

    canvas->setTextColor(TFT_ORANGE);
    canvas->drawString("RADAR", 2, 0);
    // filter box
    canvas->setFont(FONT_10);
    char fbuf[24];
    snprintf(fbuf, sizeof(fbuf), "find:%s_", _data.filter.c_str());
    canvas->setTextColor(_data.filter.empty() ? TFT_DARKGREY : TFT_CYAN);
    canvas->drawString(fbuf, 48, 1);
    canvas->setFont(FONT_12);
    canvas->drawFastHLine(0, HEADER_HEIGHT - 1, canvas->width(), THEME_COLOR_HEADER_LINE);

    if (total == 0)
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("<no nodes>", canvas->width() / 2, canvas->height() / 2 - 6);
        return;
    }

    int list_h = canvas->height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    int max_rows = std::max(1, list_h / ROW_HEIGHT);

    if (_data.selected_index < _data.scroll_offset)
        _data.scroll_offset = _data.selected_index;
    if (_data.selected_index >= _data.scroll_offset + max_rows)
        _data.scroll_offset = _data.selected_index - max_rows + 1;
    if (_data.scroll_offset < 0)
        _data.scroll_offset = 0;

    uint32_t now_s = (uint32_t)time(nullptr);

    for (int i = 0; i < max_rows && (_data.scroll_offset + i) < total; i++)
    {
        int idx = _data.scroll_offset + i;
        uint32_t nid = _data.filtered[idx];
        const Mesh::NodeIndexEntry* e = ndb ? ndb->getNodeIndex(nid) : nullptr;
        int y = HEADER_HEIGHT + i * ROW_HEIGHT;
        bool selected = (idx == _data.selected_index);

        if (selected)
            canvas->fillRect(1, y, canvas->width() - 2, ROW_HEIGHT, THEME_COLOR_BG_SELECTED);

        canvas->setFont(FONT_12);
        canvas->setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY);
        canvas->drawString(_node_label(nid).c_str(), 4, y + 1);

        if (e)
        {
            // position indicator
            bool has_pos = (e->latitude_i != 0 || e->longitude_i != 0);
            canvas->setFont(FONT_10);
            canvas->setTextColor(has_pos ? TFT_GREEN : TFT_DARKGREY);
            canvas->drawString(has_pos ? "GPS" : "---", 120, y + 2);

            char sig[20];
            snprintf(sig, sizeof(sig), "%ddBm", (int)e->last_rssi);
            canvas->setTextColor(selected ? TFT_WHITE : TFT_CYAN);
            canvas->drawRightString(sig, canvas->width() - 4, y + 2);
        }
    }

    canvas->setFont(FONT_10);
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawCenterString(SEL_HINT, canvas->width() / 2, canvas->height() - 10);
    canvas->setFont(FONT_12);
}

void AppNodeRadar::_handle_select_input()
{
    static bool is_repeat = false;
    static uint32_t next_fire_ts = 0;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (!_data.hal->keyboard()->isPressed())
    {
        is_repeat = false;
        return;
    }

    uint32_t now = (uint32_t)millis();
    auto ks = _data.hal->keyboard()->keysState();
    int total = (int)_data.filtered.size();

    if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->home_button()->is_pressed())
    {
        _data.hal->playNextSound();
        _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
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
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
    {
        if (total > 0)
        {
            _data.target_id = _data.filtered[_data.selected_index];
            _data.rssi_hist_count = 0;
            _data.prev_rssi = 0;
            _data.last_refresh_ms = 0;
            _data.view = View::RADAR;
            _data.update_view = true;
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
        }
    }
    else if (ks.del)
    {
        if (!_data.filter.empty())
        {
            _data.filter.pop_back();
            _rebuild_filtered();
            _data.update_view = true;
        }
    }
    else
    {
        for (char c : ks.values)
        {
            if (isalnum((unsigned char)c) && _data.filter.size() < FILTER_MAX)
            {
                _data.filter.push_back((char)tolower((unsigned char)c));
                _rebuild_filtered();
                _data.update_view = true;
                break;
            }
        }
    }
}

// ---------------- RADAR view ----------------

bool AppNodeRadar::_lookup_target(Mesh::NodeIndexEntry& out) const
{
    auto* ndb = _data.hal->nodedb();
    if (!ndb)
        return false;
    const Mesh::NodeIndexEntry* e = ndb->getNodeIndex(_data.target_id);
    if (!e)
        return false;
    out = *e;
    return true;
}

void AppNodeRadar::_push_rssi(int16_t rssi)
{
    if (_data.rssi_hist_count < RSSI_HIST)
        _data.rssi_hist[_data.rssi_hist_count++] = rssi;
    else
    {
        for (int i = 1; i < RSSI_HIST; i++)
            _data.rssi_hist[i - 1] = _data.rssi_hist[i];
        _data.rssi_hist[RSSI_HIST - 1] = rssi;
    }
}

void AppNodeRadar::_render_radar()
{
    auto* canvas = _data.hal->canvas();
    auto* gps = _data.hal->gps();
    canvas->fillScreen(THEME_COLOR_BG);

    Mesh::NodeIndexEntry e;
    bool found = _lookup_target(e);

    // Header
    canvas->setFont(FONT_12);
    canvas->setTextColor(TFT_ORANGE);
    canvas->drawString("RADAR", 2, 0);
    canvas->setTextColor(TFT_WHITE);
    canvas->drawRightString(_node_label(_data.target_id).c_str(), canvas->width() - 2, 0);
    canvas->drawFastHLine(0, HEADER_HEIGHT - 1, canvas->width(), THEME_COLOR_HEADER_LINE);

    if (!found)
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("node not in db", canvas->width() / 2, canvas->height() / 2 - 6);
        canvas->setFont(FONT_10);
        canvas->drawCenterString("[ESC] back", canvas->width() / 2, canvas->height() - 10);
        canvas->setFont(FONT_12);
        return;
    }

    // ---- Compass (left) ----
    int cx = 60, cy = 72, r = 38;
    bool gps_fix = gps && gps->hasFix();
    bool has_pos = (e.latitude_i != 0 || e.longitude_i != 0);
    bool moving = gps_fix && gps->getGroundSpeed() > 80; // > 0.8 m/s
    float heading = moving ? (gps->getGroundTrack() / 100000.0f) : 0.0f;

    canvas->drawCircle(cx, cy, r, THEME_COLOR_HEADER_LINE);
    canvas->drawCircle(cx, cy, r / 2, THEME_COLOR_HEADER_LINE);
    canvas->fillCircle(cx, cy, 2, TFT_DARKGREY);

    // North marker (rotated by -heading)
    float nrad = deg2rad(-heading);
    int nx = cx + (int)((r + 6) * sin(nrad));
    int ny = cy - (int)((r + 6) * cos(nrad));
    canvas->setFont(FONT_10);
    canvas->setTextColor(TFT_RED);
    canvas->drawCenterString("N", nx, ny - 5);

    if (gps_fix && has_pos)
    {
        double mylat = gps->getLatitude(), mylon = gps->getLongitude();
        double tlat = e.latitude_i / 1e7, tlon = e.longitude_i / 1e7;
        float brg = bearing_deg(mylat, mylon, tlat, tlon);
        double dist = haversine_m(mylat, mylon, tlat, tlon);
        float rel = brg - heading;
        float rad = deg2rad(rel);

        int tipx = cx + (int)(r * 0.82f * sin(rad));
        int tipy = cy - (int)(r * 0.82f * cos(rad));
        // arrow shaft (thick) + head
        for (int o = -1; o <= 1; o++)
        {
            canvas->drawLine(cx + o, cy, tipx + o, tipy, TFT_GREEN);
            canvas->drawLine(cx, cy + o, tipx, tipy + o, TFT_GREEN);
        }
        float lrad = deg2rad(rel + 152), rrad = deg2rad(rel - 152);
        canvas->fillTriangle(tipx, tipy, tipx + (int)(10 * sin(lrad)), tipy - (int)(10 * cos(lrad)),
                             tipx + (int)(10 * sin(rrad)), tipy - (int)(10 * cos(rrad)), TFT_GREEN);

        char dbuf[16];
        if (dist < 1000.0)
            snprintf(dbuf, sizeof(dbuf), "%dm", (int)dist);
        else
            snprintf(dbuf, sizeof(dbuf), "%.1fkm", dist / 1000.0);
        canvas->setFont(FONT_12);
        canvas->setTextColor(TFT_GREEN);
        canvas->drawCenterString(dbuf, cx, cy + r + 8);
        char bbuf[12];
        snprintf(bbuf, sizeof(bbuf), "%d\xC2\xB0", (int)brg);
        canvas->setFont(FONT_10);
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString(bbuf, cx, cy - r - 9);
    }
    else
    {
        canvas->setFont(FONT_12);
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("?", cx, cy - 6);
        canvas->setFont(FONT_10);
        canvas->drawCenterString(!gps_fix ? "no GPS fix" : "no node pos", cx, cy + r + 8);
    }

    // ---- Signal meter (right) ----
    int px = 116;
    int rssi = e.last_rssi;
    float snr = e.snr;
    float pct = (rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN);
    if (pct < 0)
        pct = 0;
    if (pct > 1)
        pct = 1;
    uint32_t scolor = rssi > -85 ? THEME_COLOR_SIGNAL_GOOD : (rssi > -100 ? THEME_COLOR_SIGNAL_FAIR : THEME_COLOR_SIGNAL_BAD);

    canvas->setFont(FONT_10);
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawString("SIGNAL", px, HEADER_HEIGHT + 2);

    // big rssi number
    canvas->setFont(FONT_12);
    char rbuf[16];
    snprintf(rbuf, sizeof(rbuf), "%d", rssi);
    canvas->setTextColor(scolor);
    canvas->drawString(rbuf, px, HEADER_HEIGHT + 13);
    canvas->setFont(FONT_10);
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawString("dBm", px + 40, HEADER_HEIGHT + 15);

    // trend (warmer/colder) vs previous reading
    const char* trend = "= steady";
    uint32_t tcolor = TFT_DARKGREY;
    if (_data.prev_rssi != 0)
    {
        if (rssi > _data.prev_rssi + 2)
        {
            trend = "\xE2\x96\xB2 warmer";
            tcolor = THEME_COLOR_SIGNAL_GOOD;
        }
        else if (rssi < _data.prev_rssi - 2)
        {
            trend = "\xE2\x96\xBC colder";
            tcolor = THEME_COLOR_SIGNAL_BAD;
        }
    }
    canvas->setTextColor(tcolor);
    canvas->drawString(trend, px, HEADER_HEIGHT + 28);
    _data.prev_rssi = (int16_t)rssi;

    // strength bar
    int bx = px, by = HEADER_HEIGHT + 42, bw = canvas->width() - px - 6, bh = 8;
    canvas->drawRect(bx, by, bw, bh, THEME_COLOR_HEADER_LINE);
    int fillw = (int)(pct * (bw - 2));
    if (fillw > 0)
        canvas->fillRect(bx + 1, by + 1, fillw, bh - 2, scolor);

    // snr + age
    char sbuf[24];
    uint32_t now_s = (uint32_t)time(nullptr);
    uint32_t age = (e.last_heard && now_s >= e.last_heard) ? (now_s - e.last_heard) : 0;
    snprintf(sbuf, sizeof(sbuf), "SNR %.1f  %lus", (double)snr, (unsigned long)age);
    canvas->setTextColor(TFT_CYAN);
    canvas->drawString(sbuf, px, HEADER_HEIGHT + 54);

    // rssi history sparkline
    int gx = px, gy = HEADER_HEIGHT + 66, gw = canvas->width() - px - 6, gh = 16;
    canvas->drawRect(gx, gy, gw, gh, THEME_COLOR_HEADER_LINE);
    if (_data.rssi_hist_count > 1)
    {
        int n = _data.rssi_hist_count;
        for (int i = 1; i < n; i++)
        {
            float p0 = (_data.rssi_hist[i - 1] - RSSI_MIN) / (RSSI_MAX - RSSI_MIN);
            float p1 = (_data.rssi_hist[i] - RSSI_MIN) / (RSSI_MAX - RSSI_MIN);
            p0 = p0 < 0 ? 0 : (p0 > 1 ? 1 : p0);
            p1 = p1 < 0 ? 0 : (p1 > 1 ? 1 : p1);
            int x0 = gx + 1 + (int)((float)(i - 1) / (RSSI_HIST - 1) * (gw - 2));
            int x1 = gx + 1 + (int)((float)i / (RSSI_HIST - 1) * (gw - 2));
            int y0 = gy + gh - 1 - (int)(p0 * (gh - 2));
            int y1 = gy + gh - 1 - (int)(p1 * (gh - 2));
            canvas->drawLine(x0, y0, x1, y1, THEME_COLOR_SIGNAL_GOOD);
        }
    }

    // Footer status
    canvas->setFont(FONT_10);
    canvas->setTextColor(TFT_DARKGREY);
    char foot[40];
    snprintf(foot, sizeof(foot), "GPS:%s %s  [ESC]back", gps_fix ? "fix" : "none", moving ? "HDG" : "N-up");
    canvas->drawString(foot, 2, canvas->height() - 10);
    canvas->setFont(FONT_12);
}

void AppNodeRadar::_handle_radar_input()
{
    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (!_data.hal->keyboard()->isPressed())
        return;

    if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE) ||
        _data.hal->home_button()->is_pressed())
    {
        _data.hal->playNextSound();
        _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
        // back to the picker rather than exiting the app
        _data.view = View::SELECT;
        _data.last_change_counter = 0xFFFFFFFF;
        _data.update_view = true;
    }
}
