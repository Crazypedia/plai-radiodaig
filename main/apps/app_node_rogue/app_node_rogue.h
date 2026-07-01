/**
 * @file app_node_rogue.h
 * @brief Rogue Node Tracker - flags noisy nodes by airtime/rate + collision warnings
 * @version 1.0
 * @date 2026-06-27
 *
 * Sits on top of the existing MeshDataStore packet-log ring buffer. For every
 * node heard on the air it tallies packet count, estimated LoRa time-on-air, and
 * channel-airtime share, then flags nodes that hog the channel. Also scans for
 * overlapping transmissions (potential packet collisions).
 */
#pragma once

#include "../apps.h"
#include <string>
#include <vector>

#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/icon/icon_define.h"

#include "assets/app_node_rogue.h"
#include "mesh/node_threat.h"

namespace MOONCAKE::APPS
{

    class AppNodeRogue : public APP_BASE
    {
    public:
        enum class Tab
        {
            TRAFFIC,
            SIGNAL,
            HISTORY
        };

    private:
        struct NodeStat
        {
            uint32_t node_id;
            uint32_t count;         // packets heard in window
            uint32_t airtime_ms;    // summed estimated time-on-air
            uint32_t last_ts_ms;    // most recent capture
            int16_t last_rssi;      // RSSI of most recent
            float last_snr;         // SNR of most recent
            uint32_t sum_bytes;     // raw payload bytes
            float distance_m;       // Distance from us (if known)
            uint8_t threat_flags;   // Mesh::ThreatFlag bits (per-node misbehavior)
            uint8_t max_hop_start;  // highest hop_start seen from this node
            uint16_t ack_bcast;     // want_ack-on-broadcast count
            uint16_t dup_max;       // max repeats of any one packet id (replay)
        };

        struct
        {
            HAL::Hal* hal;

            Tab current_tab;
            int selected_index;
            int scroll_offset;

            uint32_t last_generation;
            uint32_t last_refresh_ms;
            bool update_view;

            std::vector<NodeStat> stats; // sorted based on tab
            float channel_util;          // %
            uint32_t window_ms;          // span of the observed packet window
            uint32_t collisions;         // overlapping-TX events in window
            uint32_t crc_errors;         // CRC-failed packets in window
            uint32_t impersonations;     // RX packets claiming our own node id
        } _data;

        void _recompute();
        uint32_t _row_color(const NodeStat& s, bool& is_rogue) const;
        uint32_t _signal_color(const NodeStat& s, bool& is_anomaly) const;
        float _airtime_share(const NodeStat& s) const;
        float _rate_ppm(const NodeStat& s) const;
        std::string _node_label(uint32_t node_id) const;
        static int _threat_str(uint8_t flags, char* out, size_t cap);
        void _render();
        void _render_tabs();
        void _render_traffic_tab();
        void _render_signal_tab();
        void _render_history_tab();
        void _handle_input();

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
    };

    class AppNodeRogue_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "ROGUE"; }
        std::string getAppDesc() override { return "Rogue node tracker"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_node_rogue, nullptr)); }
        void* newApp() override { return new AppNodeRogue; }
        void deleteApp(void* app) override { delete (AppNodeRogue*)app; }
    };

} // namespace MOONCAKE::APPS
