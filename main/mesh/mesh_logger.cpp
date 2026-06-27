/**
 * @file mesh_logger.cpp
 * @brief Background SD logger for mesh traffic (NDJSON + LoRaTap PCAP).
 */
#include "mesh_logger.h"
#include "common_define.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <math.h>

namespace Mesh
{
    static const char* TAG = "MESHLOG";

    static const char* LOG_DIR = "/sdcard/logs";
    static const uint32_t MAX_FILE_BYTES = 16u * 1024u * 1024u; // rotate well under FAT32 4GiB
    static const uint32_t SYNC_INTERVAL_MS = 5000;
    static const uint32_t POS_INTERVAL_MS = 5000; // periodic GPS track point cadence
    static const uint16_t LORATAP_HDR_LEN = 15;
    static const uint32_t LINKTYPE_LORATAP = 270;

    static inline void put_be16(uint8_t* p, uint16_t v)
    {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
    }
    static inline void put_be32(uint8_t* p, uint32_t v)
    {
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
    }
    static inline void put_le16(uint8_t* p, uint16_t v)
    {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
    }
    static inline void put_le32(uint8_t* p, uint32_t v)
    {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    }

    MeshLogger& MeshLogger::instance()
    {
        static MeshLogger inst;
        return inst;
    }

    void MeshLogger::ensureLogDir()
    {
        if (_dir_ready)
            return;
        struct stat st;
        if (stat(LOG_DIR, &st) != 0)
            mkdir(LOG_DIR, 0775);
        _dir_ready = true;
    }

    uint32_t MeshLogger::nowEpoch()
    {
        uint32_t t = (uint32_t)time(nullptr);
        if (t > 1700000000u) // RTC looks valid
            return t;
        // No real clock yet: synthesize a monotonic epoch from the session base.
        if (_base_epoch == 0)
        {
            _base_epoch = 1; // marker: session-relative time
            _base_millis = (uint32_t)millis();
        }
        return _base_epoch + ((uint32_t)millis() - _base_millis) / 1000u;
    }

    void MeshLogger::openNdjson()
    {
        if (_ndjson)
            return;
        ensureLogDir();
        char path[64];
        snprintf(path, sizeof(path), "%s/mesh-%010u-%u.ndjson", LOG_DIR, (unsigned)nowEpoch(), (unsigned)_seq);
        _ndjson = fopen(path, "a");
        if (!_ndjson)
        {
            ESP_LOGW(TAG, "Cannot open %s (SD mounted?)", path);
            _ndjson_on = false;
            return;
        }
        static char buf[4096];
        setvbuf(_ndjson, buf, _IOFBF, sizeof(buf));
        _ndjson_bytes = (uint32_t)ftell(_ndjson);
        ESP_LOGI(TAG, "NDJSON logging -> %s", path);
    }

    void MeshLogger::openPcap()
    {
        if (_pcap)
            return;
        ensureLogDir();
        char path[64];
        snprintf(path, sizeof(path), "%s/mesh-%010u-%u.pcap", LOG_DIR, (unsigned)nowEpoch(), (unsigned)_seq);
        _pcap = fopen(path, "ab");
        if (!_pcap)
        {
            ESP_LOGW(TAG, "Cannot open %s (SD mounted?)", path);
            _pcap_on = false;
            return;
        }
        static char pbuf[4096];
        setvbuf(_pcap, pbuf, _IOFBF, sizeof(pbuf));
        _pcap_bytes = (uint32_t)ftell(_pcap);
        if (_pcap_bytes == 0)
        {
            // Classic libpcap global header (little-endian).
            uint8_t gh[24];
            put_le32(gh + 0, 0xa1b2c3d4); // magic
            put_le16(gh + 4, 2);          // version major
            put_le16(gh + 6, 4);          // version minor
            put_le32(gh + 8, 0);          // thiszone
            put_le32(gh + 12, 0);         // sigfigs
            put_le32(gh + 16, 65535);     // snaplen
            put_le32(gh + 20, LINKTYPE_LORATAP);
            fwrite(gh, 1, sizeof(gh), _pcap);
            _pcap_bytes += sizeof(gh);
        }
        ESP_LOGI(TAG, "PCAP (LoRaTap) logging -> %s", path);
    }

    void MeshLogger::closeNdjson()
    {
        if (_ndjson)
        {
            fclose(_ndjson);
            _ndjson = nullptr;
        }
    }

    void MeshLogger::closePcap()
    {
        if (_pcap)
        {
            fclose(_pcap);
            _pcap = nullptr;
        }
    }

    void MeshLogger::configure(bool ndjson_enabled, bool pcap_enabled)
    {
        if (ndjson_enabled && !_ndjson)
            openNdjson();
        else if (!ndjson_enabled && _ndjson)
            closeNdjson();

        if (pcap_enabled && !_pcap)
            openPcap();
        else if (!pcap_enabled && _pcap)
            closePcap();

        _ndjson_on = ndjson_enabled && _ndjson != nullptr;
        _pcap_on = pcap_enabled && _pcap != nullptr;
    }

    void MeshLogger::setPosition(double lat, double lon, int32_t alt, uint8_t sats, bool fix)
    {
        _lat = lat;
        _lon = lon;
        _alt = alt;
        _sats = sats;
        _fix = fix;
    }

    size_t MeshLogger::positionSuffix(char* dst, size_t cap)
    {
        if (_fix)
            return (size_t)snprintf(dst, cap, ",\"rx_fix\":true,\"rx_lat\":%.6f,\"rx_lon\":%.6f,\"rx_alt\":%d,\"rx_sats\":%u",
                                    _lat, _lon, (int)_alt, (unsigned)_sats);
        return (size_t)snprintf(dst, cap, ",\"rx_fix\":false");
    }

    // Append c to dst as a JSON-escaped char (dst must have room; bounded by caller).
    static size_t json_escape(char* dst, size_t cap, const char* src)
    {
        size_t o = 0;
        for (const char* s = src; *s && o + 6 < cap; s++)
        {
            unsigned char c = (unsigned char)*s;
            if (c == '"' || c == '\\')
            {
                dst[o++] = '\\';
                dst[o++] = (char)c;
            }
            else if (c == '\n')
            {
                dst[o++] = '\\';
                dst[o++] = 'n';
            }
            else if (c == '\r')
            {
                dst[o++] = '\\';
                dst[o++] = 'r';
            }
            else if (c < 0x20)
            {
                o += snprintf(dst + o, cap - o, "\\u%04x", c);
            }
            else
            {
                dst[o++] = (char)c;
            }
        }
        dst[o] = '\0';
        return o;
    }

    void MeshLogger::logEntry(const PacketLogEntry& e)
    {
        if (!_ndjson_on || !_ndjson)
            return;

        char desc[120];
        json_escape(desc, sizeof(desc), e.payload_desc);
        char pos[96];
        positionSuffix(pos, sizeof(pos));

        int n = fprintf(_ndjson,
                        "{\"type\":\"pkt\",\"t\":%u,\"ms\":%u,\"dir\":\"%s\",\"from\":\"!%08x\",\"to\":\"!%08x\",\"id\":%u,"
                        "\"port\":%u,\"size\":%u,\"rssi\":%d,\"snr\":%.2f,\"hop_start\":%u,\"hop_limit\":%u,"
                        "\"ch\":%u,\"relay\":%u,\"want_ack\":%s,\"via_mqtt\":%s,\"decoded\":%s,\"crc_err\":%s,"
                        "\"desc\":\"%s\"%s}\n",
                        (unsigned)nowEpoch(), (unsigned)e.timestamp_ms, e.is_tx ? "tx" : "rx", (unsigned)e.from,
                        (unsigned)e.to, (unsigned)e.id, (unsigned)e.port, (unsigned)e.size, (int)e.rssi, (double)e.snr,
                        (unsigned)e.hop_start, (unsigned)e.hop_limit, (unsigned)e.channel, (unsigned)e.relay_node,
                        e.want_ack ? "true" : "false", e.via_mqtt ? "true" : "false", e.decoded ? "true" : "false",
                        e.crc_error ? "true" : "false", desc, pos);
        if (n > 0)
            _ndjson_bytes += (uint32_t)n;

        if (_ndjson_bytes >= MAX_FILE_BYTES)
        {
            closeNdjson();
            _seq++;
            openNdjson();
        }
    }

    void MeshLogger::logRaw(uint32_t ts_ms, int16_t rssi, float snr, uint32_t freq_hz, uint8_t sf, float bw_khz,
                            const uint8_t* frame, size_t len)
    {
        (void)ts_ms;
        if (!_pcap_on || !_pcap || !frame || len == 0)
            return;

        uint32_t epoch = nowEpoch();
        uint32_t usec = ((uint32_t)millis() % 1000u) * 1000u;
        uint32_t incl = LORATAP_HDR_LEN + (uint32_t)len;

        uint8_t rec[16];
        put_le32(rec + 0, epoch);
        put_le32(rec + 4, usec);
        put_le32(rec + 8, incl);
        put_le32(rec + 12, incl);
        fwrite(rec, 1, sizeof(rec), _pcap);

        // LoRaTap v0 header (big-endian multi-byte fields).
        uint8_t lt[LORATAP_HDR_LEN];
        memset(lt, 0, sizeof(lt));
        lt[0] = 0; // version
        lt[1] = 0; // padding
        put_be16(lt + 2, LORATAP_HDR_LEN);
        put_be32(lt + 4, freq_hz);
        int bwc = (int)(bw_khz / 125.0f + 0.5f);
        lt[8] = (uint8_t)(bwc < 1 ? 1 : bwc);
        lt[9] = sf;
        int rv = rssi + 139;
        rv = rv < 0 ? 0 : (rv > 255 ? 255 : rv);
        lt[10] = (uint8_t)rv; // packet rssi
        lt[11] = 0;           // max rssi
        lt[12] = (uint8_t)rv; // current rssi
        lt[13] = (uint8_t)(int8_t)lroundf(snr * 4.0f);
        lt[14] = 0x2b; // Meshtastic LoRa sync word
        fwrite(lt, 1, sizeof(lt), _pcap);

        fwrite(frame, 1, len, _pcap);
        _pcap_bytes += sizeof(rec) + sizeof(lt) + (uint32_t)len;

        if (_pcap_bytes >= MAX_FILE_BYTES)
        {
            closePcap();
            _seq++;
            openPcap();
        }
    }

    void MeshLogger::tick()
    {
        if (!_ndjson && !_pcap)
            return;
        uint32_t now = (uint32_t)millis();

        // Periodic GPS track point so the walk path stays continuous even with no
        // packets (correlate to packet records by time).
        if (_ndjson_on && _ndjson && _fix && (now - _last_pos_ms >= POS_INTERVAL_MS))
        {
            _last_pos_ms = now;
            int n = fprintf(_ndjson,
                            "{\"type\":\"pos\",\"t\":%u,\"ms\":%u,\"rx_lat\":%.6f,\"rx_lon\":%.6f,\"rx_alt\":%d,"
                            "\"rx_sats\":%u}\n",
                            (unsigned)nowEpoch(), (unsigned)now, _lat, _lon, (int)_alt, (unsigned)_sats);
            if (n > 0)
                _ndjson_bytes += (uint32_t)n;
        }

        if (now - _last_sync_ms < SYNC_INTERVAL_MS)
            return;
        _last_sync_ms = now;
        if (_ndjson)
        {
            fflush(_ndjson);
            fsync(fileno(_ndjson));
        }
        if (_pcap)
        {
            fflush(_pcap);
            fsync(fileno(_pcap));
        }
    }

} // namespace Mesh
