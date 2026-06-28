/**
 * @file mesh_logger.h
 * @brief Background SD logger for mesh traffic. Writes an NDJSON event log
 *        (decoded metadata + payload summary) and, optionally, a LoRaTap PCAP
 *        of raw on-air frames for Wireshark. Runs in the mesh service context,
 *        independent of which app is on screen.
 *
 * Format/method rationale (low data rate, long logs, FAT32):
 *  - NDJSON: one JSON object per line, append-friendly, DuckDB/pandas/jq ready.
 *  - PCAP:  LoRaTap link-type (DLT 270), opens in Wireshark w/ Meshtastic dissector.
 *  - File handle kept open + sequential append; periodic fsync (not per record);
 *    size rotation well under FAT32's 4 GiB limit.
 *  - Files are named by calendar day (mesh-YYYYMMDD-<seq>.*); a power cycle
 *    appends to that day's file instead of starting a new one. A new <seq>
 *    segment is only opened when the day rolls over or the current segment
 *    hits the size limit.
 *  - Before GPS has supplied a valid time, entries go to mesh-pending-<seq>.*
 *    so nothing is lost or mis-dated; once time syncs, new writes switch to
 *    that day's file (the pending file is left on disk as-is).
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "mesh_data.h"

namespace Mesh
{

    class MeshLogger
    {
    public:
        static MeshLogger& instance();

        // Enable/disable each format; opens or closes the SD files as needed.
        void configure(bool ndjson_enabled, bool pcap_enabled);

        bool anyEnabled() const { return _ndjson_on || _pcap_on; }

        // NDJSON: one line per logged packet (RX/TX/CRC). Hook in addPacketLogEntry.
        void logEntry(const PacketLogEntry& e);

        // PCAP: a raw on-air frame (LoRaTap). Hook in the radio RX path.
        void logRaw(uint32_t ts_ms, int16_t rssi, float snr, uint32_t freq_hz, uint8_t sf, float bw_khz,
                    const uint8_t* frame, size_t len);

        // Cache the receiver's (our) GPS position; embedded in NDJSON records and
        // emitted as periodic track points for walk-test geo-correlation.
        void setPosition(double lat, double lon, int32_t alt, uint8_t sats, bool fix);

        // Periodic flush to bound power-loss; call from the mesh update loop.
        void tick();

    private:
        MeshLogger() = default;
        size_t positionSuffix(char* dst, size_t cap);

        void openNdjson();
        void openPcap();
        void openNdjsonFile(uint32_t seq);
        void openPcapFile(uint32_t seq);
        void closeNdjson();
        void closePcap();
        void ensureLogDir();
        uint32_t nowEpoch();

        // Re-checks whether the active file's name (day, or pending-vs-dated)
        // still matches the current clock; rotates to a new segment if not.
        void maybeRotate();

        // Picks up where a previous boot/segment left off: finds the highest
        // existing <prefix>-<seq>.<ext> file and reuses it if under the size
        // limit, otherwise starts the next seq with size 0.
        void resolveSeqAndSize(const char* prefix, const char* ext, uint32_t& seq_out, uint32_t& size_out);

        // "mesh-YYYYMMDD" once time is valid, else "mesh-pending".
        static void formatPrefix(char* out, size_t cap, bool synced);

        bool _ndjson_on = false;
        bool _pcap_on = false;
        FILE* _ndjson = nullptr;
        FILE* _pcap = nullptr;
        uint32_t _ndjson_bytes = 0;
        uint32_t _pcap_bytes = 0;
        uint32_t _ndjson_seq = 0;
        uint32_t _pcap_seq = 0;
        char _ndjson_prefix[24] = "";
        char _pcap_prefix[24] = "";
        uint32_t _base_epoch = 0;
        uint32_t _base_millis = 0;
        uint32_t _last_sync_ms = 0;
        uint32_t _last_pos_ms = 0;
        bool _dir_ready = false;

        // Receiver GPS position (our own), most recent.
        double _lat = 0.0, _lon = 0.0;
        int32_t _alt = 0;
        uint8_t _sats = 0;
        bool _fix = false;
    };

} // namespace Mesh
