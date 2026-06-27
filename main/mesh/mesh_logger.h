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
        void closeNdjson();
        void closePcap();
        void ensureLogDir();
        uint32_t nowEpoch();

        bool _ndjson_on = false;
        bool _pcap_on = false;
        FILE* _ndjson = nullptr;
        FILE* _pcap = nullptr;
        uint32_t _ndjson_bytes = 0;
        uint32_t _pcap_bytes = 0;
        uint32_t _seq = 0;
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
