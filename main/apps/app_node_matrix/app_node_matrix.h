/**
 * @file app_node_matrix.h
 * @brief Node Matrix widget - heat-map grid of known nodes by recency/signal
 * @version 1.0
 * @date 2026-06-27
 *
 */
#pragma once

#include "../apps.h"
#include <string>
#include <vector>

#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/icon/icon_define.h"
#include "mesh/node_db.h"

#include "assets/app_node_matrix.h"

namespace MOONCAKE::APPS
{

    class AppNodeMatrix : public APP_BASE
    {
    private:
        struct
        {
            HAL::Hal* hal;

            int cols;
            int rows;
            int cell_size;
            int grid_x;
            int grid_y;

            int selected_index; // index into the filtered node list (0 = most recently heard)
            int scroll_offset;  // first visible cell index (multiple of cols)

            // Type-to-filter search (matches short/long name and node id)
            std::string filter;
            std::vector<Mesh::NodeIndexEntry> nodes; // sorted + filtered working set

            uint32_t last_change_counter;
            bool update_view;
        } _data;

        void _layout_grid();
        void _rebuild_nodes(); // re-sort and re-filter into _data.nodes
        uint32_t _cell_color(const Mesh::NodeIndexEntry& entry) const;
        void _render();
        void _handle_input();

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
    };

    class AppNodeMatrix_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "MATRIX"; }
        std::string getAppDesc() override { return "Node heat-map"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_app_node_matrix, nullptr)); }
        void* newApp() override { return new AppNodeMatrix; }
        void deleteApp(void* app) override { delete (AppNodeMatrix*)app; }
    };

} // namespace MOONCAKE::APPS
