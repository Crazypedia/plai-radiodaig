/**
 * @file app_log_reader.h
 * @brief Offline reader for the SD NDJSON mesh logs written by Mesh::MeshLogger.
 *        Browses the .ndjson files in /sdcard/logs, parses the most recent
 *        records of a chosen file back into PacketLogEntry, and reuses the live
 *        Monitor's packet-list / packet-detail views to inspect past traffic.
 *
 *        Memory note: PSRAM is not enabled, so files are NOT loaded whole. We
 *        tail-read the last TAIL_BYTES of the file and keep at most MAX_ENTRIES
 *        of the newest records (see app_log_reader.cpp).
 */
#pragma once

#include "../apps.h"
#include <string>
#include <vector>

#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/icon/icon_define.h"
#include "apps/utils/anim/hl_text.h"
#include "mesh/mesh_data.h"

// Own icon copy (same image as Monitor for now; swap the data to differentiate).
#include "assets/app_log_reader.h"

namespace MOONCAKE::APPS
{

    class AppLogReader : public APP_BASE
    {
    public:
        enum class ViewState
        {
            FILE_LIST,
            PACKET_LIST,
            PACKET_DETAIL
        };

        // One parsed NDJSON "pkt" record: the decoded fields plus the absolute
        // epoch from the log (the live struct only carries session-relative ms).
        struct ReaderEntry
        {
            Mesh::PacketLogEntry pkt;
            uint32_t epoch;
        };

        struct FileItem
        {
            std::string name; // basename under /sdcard/logs
            uint32_t size;    // bytes
        };

    private:
        struct
        {
            HAL::Hal* hal;
            ViewState view_state;

            // File picker
            std::vector<FileItem> files;
            int file_index;
            int file_scroll;
            bool update_files;

            // Loaded packets (chronological: [0]=oldest, [size-1]=newest)
            std::vector<ReaderEntry> entries;

            // Packet list state
            int selected_index;
            int scroll_offset;
            bool update_list;
            bool packet_list_ctrl;

            // Detail state
            Mesh::PacketLogEntry detail_pkt;
            uint32_t detail_epoch;
            uint32_t detail_delta_ms;
            int detail_scroll;
            int detail_scroll_max;

            // Animation
            UTILS::HL_TEXT::HLTextContext_t hint_hl_ctx;
        } _data;

        // File handling
        void _scan_files();
        bool _load_file(const std::string& name);

        // Rendering
        bool _render_file_list();
        bool _render_packet_list();
        bool _render_packet_detail();
        bool _render_file_hint();
        bool _render_list_hint();
        bool _render_detail_hint();

        // Input
        void _handle_file_input();
        void _handle_list_input();
        void _handle_detail_input();

        // Helpers
        const char* _port_name(uint8_t port);
        const char* _direction_str(const Mesh::PacketLogEntry& pkt);
        uint32_t _direction_color(const Mesh::PacketLogEntry& pkt);

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
    };

    class AppLogReader_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "LOG READER"; }
        std::string getAppDesc() override { return "Mesh log file reader"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_log_reader, nullptr)); }
        void* newApp() override { return new AppLogReader; }
        void deleteApp(void* app) override { delete (AppLogReader*)app; }
    };

} // namespace MOONCAKE::APPS
