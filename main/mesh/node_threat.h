/**
 * @file node_threat.h
 * @brief Shared misbehaving/malicious-node detection primitives.
 *
 * Both the live Rogue Node Tracker (over the RAM packet ring) and the boot-time
 * historical back-fill (over the 2 h SD-log window) feed the same flag logic and
 * thresholds so the two views stay consistent. Everything here is fixed-size to
 * keep heap/CPU bounded on the no-PSRAM target.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace Mesh
{
    /**
     * @brief Per-node threat flag bits. Each has a distinct one-char indicator.
     */
    enum ThreatFlag : uint8_t
    {
        THREAT_NONE        = 0,
        THREAT_IMPERSONATE = 1 << 0, // 'I' RX packet claiming our own node id (spoof)
        THREAT_HOP_ABUSE   = 1 << 1, // 'H' hop_start above the sane mesh maximum
        THREAT_RELAY_FLOOD = 1 << 2, // 'R' relays a disproportionate share of traffic
        THREAT_ACK_AMP     = 1 << 3, // 'A' want_ack set on broadcast (ACK-storm amplifier)
        THREAT_REPLAY      = 1 << 4, // 'D' same packet id re-injected (duplicate/replay flood)
    };

    // Detection thresholds (shared between live and historical paths).
    constexpr uint8_t  THREAT_MAX_SANE_HOPS = 7;    // hop_start > this is non-conformant
    constexpr uint16_t THREAT_REPLAY_REPEATS = 4;   // (from,id) seen more than this = replay
    constexpr uint16_t THREAT_ACK_MIN = 3;          // broadcast want_ack count to flag
    constexpr uint16_t THREAT_RELAY_MIN = 10;       // min relays before judging relay share
    constexpr float    THREAT_RELAY_SHARE = 0.5f;   // >50% of relayed traffic via one node

    /**
     * @brief Bounded recent-(from,id) tracker for replay/duplicate detection.
     *
     * Fixed-size ring of exact (from,id) pairs - no hashing, so no false
     * accusations from hash collisions. observe() returns how many times the
     * pair has been seen within the recent window (1 = first sighting).
     */
    template <size_t N>
    struct ReplayTracker
    {
        struct Slot
        {
            uint32_t from;
            uint32_t id;
            uint16_t count;
            bool used;
        };
        Slot slots[N] = {};
        size_t head = 0;

        uint16_t observe(uint32_t from, uint32_t id)
        {
            for (size_t i = 0; i < N; i++)
            {
                if (slots[i].used && slots[i].from == from && slots[i].id == id)
                {
                    if (slots[i].count < 0xFFFF)
                        slots[i].count++;
                    return slots[i].count;
                }
            }
            slots[head].from = from;
            slots[head].id = id;
            slots[head].count = 1;
            slots[head].used = true;
            head = (head + 1) % N;
            return 1;
        }
    };

    // Number of recent packets tracked for replay detection.
    constexpr size_t REPLAY_TRACK_SLOTS = 96;

    /**
     * @brief Compact historical-offender record (stored snapshot from back-fill).
     */
    struct ThreatOffender
    {
        uint32_t node_id;
        uint32_t count;         // packets heard in the window
        uint8_t  flags;         // ThreatFlag bits
        uint8_t  max_hop_start; // highest hop_start observed
        uint16_t ack_bcast;     // want_ack-on-broadcast count
        uint16_t dup_max;       // max repeats of any single packet id
    };

    // Cap on stored historical offenders (top-N by packet count).
    constexpr size_t MAX_THREAT_OFFENDERS = 16;

} // namespace Mesh
