/**
 * @file app_node_matrix.cpp
 * @brief Node Matrix widget - heat-map grid of known nodes by recency/signal
 * @version 1.0
 * @date 2026-06-27
 *
 */
#include "app_node_matrix.h"
#include "common_define.h"
#include "apps/utils/theme/theme_define.h"
#include "apps/utils/ui/key_repeat.h"
#include "mesh/mesh_service.h"
#include <time.h>
#include <algorithm>

using namespace MOONCAKE::APPS;

#define CELL_SIZE 12
#define CELL_GAP 1
#define HEADER_HEIGHT 11
#define FOOTER_HEIGHT 19

// Age thresholds (seconds) for heat-map coloring
#define AGE_GOOD_SEC (5 * 60)
#define AGE_FAIR_SEC (30 * 60)
#define AGE_BAD_SEC (2 * 60 * 60)

static const char* HINT = "[↑][↓][←][→] [ESC]";

void AppNodeMatrix::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.selected_index = 0;
    _data.scroll_offset = 0;
    _data.last_change_counter = 0;
    _data.update_view = true;
    _layout_grid();
}

void AppNodeMatrix::onResume()
{
    ANIM_APP_OPEN();
    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas()->setFont(FONT_12);
    _data.hal->canvas()->setTextSize(1);
    _data.hal->canvas_update();

    _data.selected_index = 0;
    _data.scroll_offset = 0;
    _data.last_change_counter = 0xFFFFFFFF; // force first render
    _data.update_view = true;
    _layout_grid();
}

void AppNodeMatrix::onRunning()
{
    auto* nodedb = _data.hal->nodedb();
    if (nodedb)
    {
        uint32_t cc = nodedb->getChangeCounter();
        if (cc != _data.last_change_counter)
        {
            _data.last_change_counter = cc;
            _data.update_view = true;
        }
    }

    if (_data.update_view)
    {
        _render();
        _data.hal->canvas_update();
        _data.update_view = false;
    }

    _handle_input();
}

void AppNodeMatrix::onDestroy() {}

void AppNodeMatrix::_layout_grid()
{
    auto* canvas = _data.hal->canvas();
    int grid_w = canvas->width();
    int grid_h = canvas->height() - HEADER_HEIGHT - FOOTER_HEIGHT;

    _data.cell_size = CELL_SIZE;
    _data.cols = std::max(1, grid_w / (_data.cell_size + CELL_GAP));
    _data.rows = std::max(1, grid_h / (_data.cell_size + CELL_GAP));

    int used_w = _data.cols * (_data.cell_size + CELL_GAP) - CELL_GAP;
    _data.grid_x = (canvas->width() - used_w) / 2;
    _data.grid_y = HEADER_HEIGHT;
}

uint32_t AppNodeMatrix::_cell_color(const Mesh::NodeIndexEntry& entry) const
{
    if (entry.last_heard == 0)
        return THEME_COLOR_BG_DARK;

    uint32_t now = (uint32_t)time(nullptr);
    uint32_t age = (now >= entry.last_heard) ? (now - entry.last_heard) : 0;

    if (age <= AGE_GOOD_SEC)
        return THEME_COLOR_SIGNAL_GOOD;
    if (age <= AGE_FAIR_SEC)
        return THEME_COLOR_SIGNAL_FAIR;
    if (age <= AGE_BAD_SEC)
        return THEME_COLOR_SIGNAL_BAD;
    return THEME_COLOR_SIGNAL_NONE;
}

void AppNodeMatrix::_render()
{
    auto* canvas = _data.hal->canvas();
    auto* nodedb = _data.hal->nodedb();
    canvas->fillScreen(THEME_COLOR_BG);
    canvas->setFont(FONT_12);

    if (!nodedb)
        return;

    nodedb->sortIndex(Mesh::SortOrder::LAST_HEARD);
    const auto& index = nodedb->getIndex();
    int total = (int)index.size();

    // Header
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "MATRIX  %d nodes", total);
    canvas->setTextColor(TFT_ORANGE);
    canvas->drawString(hdr, 2, 0);
    canvas->drawFastHLine(0, HEADER_HEIGHT - 1, canvas->width(), THEME_COLOR_HEADER_LINE);

    if (total == 0)
    {
        canvas->setTextColor(TFT_DARKGREY);
        canvas->drawCenterString("<no nodes>", canvas->width() / 2, canvas->height() / 2 - 6);
        return;
    }

    if (_data.selected_index >= total)
        _data.selected_index = total - 1;
    if (_data.selected_index < 0)
        _data.selected_index = 0;

    int per_page = _data.cols * _data.rows;
    if (_data.selected_index < _data.scroll_offset)
        _data.scroll_offset = (_data.selected_index / _data.cols) * _data.cols;
    if (_data.selected_index >= _data.scroll_offset + per_page)
        _data.scroll_offset = (_data.selected_index / _data.cols - _data.rows + 1) * _data.cols;
    if (_data.scroll_offset < 0)
        _data.scroll_offset = 0;

    // Draw grid cells
    for (int i = 0; i < per_page; i++)
    {
        int node_idx = _data.scroll_offset + i;
        if (node_idx >= total)
            break;

        int col = i % _data.cols;
        int row = i / _data.cols;
        int x = _data.grid_x + col * (_data.cell_size + CELL_GAP);
        int y = _data.grid_y + row * (_data.cell_size + CELL_GAP);

        const auto& entry = index[node_idx];
        uint32_t color = _cell_color(entry);
        canvas->fillRect(x, y, _data.cell_size, _data.cell_size, color);

        if (node_idx == _data.selected_index)
            canvas->drawRect(x - 1, y - 1, _data.cell_size + 2, _data.cell_size + 2, TFT_WHITE);

        if (entry.flags & Mesh::NodeIndexEntry::IS_FAVORITE)
            canvas->fillRect(x + _data.cell_size - 3, y, 3, 3, THEME_COLOR_FAVORITE);
    }

    // Selected node info bar
    const auto& sel = index[_data.selected_index];
    int info_y = canvas->height() - FOOTER_HEIGHT;
    canvas->drawFastHLine(0, info_y, canvas->width(), THEME_COLOR_HEADER_LINE);

    std::string label = Mesh::NodeDB::getIndexLabel(sel);
    canvas->setFont(FONT_10);
    canvas->setTextColor(TFT_WHITE);
    canvas->drawString(label.c_str(), 2, info_y + 1);

    char age_buf[16];
    if (sel.last_heard == 0)
        snprintf(age_buf, sizeof(age_buf), "never");
    else
    {
        uint32_t now = (uint32_t)time(nullptr);
        uint32_t age = (now >= sel.last_heard) ? (now - sel.last_heard) : 0;
        if (age < 60)
            snprintf(age_buf, sizeof(age_buf), "%lus ago", (unsigned long)age);
        else if (age < 3600)
            snprintf(age_buf, sizeof(age_buf), "%lum ago", (unsigned long)(age / 60));
        else if (age < 86400)
            snprintf(age_buf, sizeof(age_buf), "%luh ago", (unsigned long)(age / 3600));
        else
            snprintf(age_buf, sizeof(age_buf), "%lud ago", (unsigned long)(age / 86400));
    }
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawString(age_buf, 44, info_y + 1);

    char sig_buf[24];
    snprintf(sig_buf, sizeof(sig_buf), "%ddBm %.1fdB", (int)sel.last_rssi, sel.snr);
    canvas->setTextColor(TFT_CYAN);
    canvas->drawRightString(sig_buf, canvas->width() - 2, info_y + 1);
    canvas->setFont(FONT_12);

    // Hint
    canvas->setFont(FONT_10);
    canvas->setTextColor(TFT_DARKGREY);
    canvas->drawCenterString(HINT, canvas->width() / 2, canvas->height() - 9);
    canvas->setFont(FONT_12);
}

void AppNodeMatrix::_handle_input()
{
    static bool is_repeat = false;
    static uint32_t next_fire_ts = 0;

    auto* nodedb = _data.hal->nodedb();
    int total = nodedb ? (int)nodedb->getIndex().size() : 0;

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
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index < total - 1)
        {
            _data.selected_index++;
            _data.hal->playNextSound();
            _data.update_view = true;
        }
    }
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index > 0)
        {
            _data.selected_index--;
            _data.hal->playNextSound();
            _data.update_view = true;
        }
    }
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index + _data.cols < total)
        {
            _data.selected_index += _data.cols;
            _data.hal->playNextSound();
            _data.update_view = true;
        }
    }
    else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
    {
        if (key_repeat_check(is_repeat, next_fire_ts, now) && _data.selected_index - _data.cols >= 0)
        {
            _data.selected_index -= _data.cols;
            _data.hal->playNextSound();
            _data.update_view = true;
        }
    }
}
