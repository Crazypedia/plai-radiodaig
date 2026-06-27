/**
 * @file app_node_radar.h
 * @brief Node Radar - pick a node, then home in on it via geographic bearing
 *        (GPS) + a live RSSI/SNR signal-strength meter.
 * @version 1.0
 * @date 2026-06-27
 *
 * Two views: SELECT (scrollable node list with live type-to-filter) and RADAR
 * (compass arrow to the node's shared GPS position + warmer/colder signal meter).
 * Heading reference is north-up, auto-rotating to GPS course-over-ground while
 * moving. No IMU yet (BMI270 tilt arrow is a future Phase B).
 */
#pragma once

#include "../apps.h"
#include <string>
#include <vector>

#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/icon/icon_define.h"
#include "mesh/node_db.h"

#include "assets/app_node_radar.h"

namespace MOONCAKE::APPS
{

    class AppNodeRadar : public APP_BASE
    {
    private:
        enum class View
        {
            SELECT,
            RADAR
        };

        static constexpr int RSSI_HIST = 32;

        struct
        {
            HAL::Hal* hal;
            View view;

            // SELECT view
            std::string filter;
            std::vector<uint32_t> filtered; // node_ids passing the filter
            int selected_index;
            int scroll_offset;
            uint32_t last_change_counter;

            // RADAR view
            uint32_t target_id;
            int16_t rssi_hist[RSSI_HIST];
            int rssi_hist_count;
            int16_t prev_rssi;

            uint32_t last_refresh_ms;
            bool update_view;
        } _data;

        // SELECT
        void _rebuild_filtered();
        void _render_select();
        void _handle_select_input();

        // RADAR
        bool _lookup_target(Mesh::NodeIndexEntry& out) const;
        void _push_rssi(int16_t rssi);
        void _render_radar();
        void _handle_radar_input();

        std::string _node_label(uint32_t node_id) const;

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
    };

    class AppNodeRadar_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "RADAR"; }
        std::string getAppDesc() override { return "Node radar / signal finder"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_node_radar, nullptr)); }
        void* newApp() override { return new AppNodeRadar; }
        void deleteApp(void* app) override { delete (AppNodeRadar*)app; }
    };

} // namespace MOONCAKE::APPS
