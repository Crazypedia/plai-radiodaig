/**
 * @file mesh_log_utils.h
 * @brief Utilities for parsing and loading Meshtastic NDJSON logs.
 */
#pragma once

#include "mesh_data.h"
#include <stdint.h>

namespace Mesh {

    /**
     * @brief A single entry from a mesh log, combining radio metadata and absolute time.
     */
    struct LogEntry {
        PacketLogEntry pkt;
        uint32_t epoch;
    };

    /**
     * @brief Parse one NDJSON line from a mesh log into a LogEntry.
     * @param line The NDJSON string to parse.
     * @param e Output LogEntry.
     * @return true if the line was a "pkt" type and parsed successfully.
     */
    bool parse_log_line(const char* line, LogEntry& e);

    /**
     * @brief Scan SD logs and load recent history into MeshDataStore time-series.
     *
     * Also builds a compact historical threat summary (top offenders + flags)
     * over the same window, stored in MeshDataStore for the Rogue app's history view.
     *
     * @param window_seconds How far back from "now" to look (e.g., 7200 for 2 hours).
     * @param our_id Our own node id, used to detect impersonation (0 = unknown/skip).
     */
    void load_recent_history(uint32_t window_seconds, uint32_t our_id = 0);

} // namespace Mesh
