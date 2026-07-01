# Proposal: Mesh Troubleshooting Tools for Plai, inspired by `mesh-metrics`

**Location:** `proposals/` (the `docs/` tree is gitignored in this fork)
**Status:** Draft / for discussion
**Scope:** New on-device diagnostic tooling for the Plai RadioDiag fork
**Source of ideas:** Review of the [`mesh-metrics`](https://github.com/cordelster/mesh-metrics) project

---

## 1. Background: what `mesh-metrics` does

`mesh-metrics` is an **infrastructure-side** telemetry collector for Meshtastic
repeater networks. A computer with a serial/IP-attached Meshtastic radio runs a
poller (`mesh_metrics.sh` or the newer `meshmetricsd.py` daemon) that walks a CSV
device list and, for each repeater, actively issues:

```
meshtastic --port /dev/ttyACM0 --request-telemetry --dest !2f67c123
```

The returned values are reshaped into Prometheus `node_exporter` exposition
format and either written to a textfile-collector directory or pushed to a
Pushgateway / VictoriaMetrics endpoint, then visualized in Grafana.

### Capabilities inventory

| Area | `mesh-metrics` capability |
|------|---------------------------|
| **Active polling** | On-demand `--request-telemetry` sweep of a **watchlist** of repeater node IDs, with a configurable per-node dwell time |
| **Metrics collected** | `Battery_level`, `Voltage`, `Total_channel_utilization` (chUtil), `Transmit_air_utilization` (airUtilTx), `uptime`, and environment metrics `temperature`, `humidity`, `pressure` |
| **Liveness** | Emits a per-node `meshtastic_up{node} 1|0` "reachable this poll" metric |
| **Roster format** | CSV: `NodeID,Contact,Location,Latitude,Longitude` — optionally AES‑256‑CBC / PBKDF2 encrypted (OpenSSL) |
| **Alerting** | Prometheus rules: battery `< 30` (warn) / `< 15` (crit); chUtil `> 60` (warn) / `> 70` (crit) |
| **Geo** | Grafana geomap with per-node lat/lon; alerting nodes turn **red** (via Alertmanager) |
| **Output** | `node_exporter` textfile `.prom`, Pushgateway POST, per-node or single-file, atomic writes |
| **Daemon** | Poll interval, stats JSON (`total_polls`, `successful_polls`, `nodes_successful`, ...) |

The crucial framing: **`mesh-metrics` is the base-station/backend half.** Plai is
the **field half** — a standalone, phone-free CardPuter terminal. Several
`mesh-metrics` features have no field equivalent in Plai today, and porting them
gives a mobile operator the same diagnostics without a laptop, Prometheus, or
Grafana.

---

## 2. What Plai already has (gap analysis)

Plai's RadioDiag fork is already strong on **passive** analysis:

- **Monitor** — live packet feed (last 50), port labels, channel hash, SNR.
- **Stats** — tabbed node/system/radio/nodeDB/GPS/mesh with port distribution.
- **Graphs** — history of **own battery**, **channel activity**, and **per-node RSSI**.
- **Matrix / Radar** — node recency heat-map and bearing/signal homing.
- **Rogue** — misbehaving-node detection (impersonation, hop abuse, relay flood, ACK amplification, replay) over both the RAM ring and a 2 h SD back-fill (`node_threat.h`).
- **Log Reader** — offline NDJSON SD log browser.
- Telemetry handling: Plai **broadcasts its own** `DeviceMetrics` (chUtil, airUtilTx) and **passively renders received** telemetry in node detail (`app_nodes.cpp` shows `channel_utilization` and `air_util_tx`).

**The gap:** everything telemetry-related in Plai is *passive* — it shows what
happens to arrive. There is **no active telemetry request** to a chosen
destination (no `TELEMETRY_APP` request-with-`want_response` to a `--dest`), **no
watchlist of repeaters to health-check**, **no threshold alerting**, and **no
environment-metric history**. That is precisely the half `mesh-metrics` owns.

---

## 3. Proposed tools (ranked)

Each proposal notes the **data source**, **reuse** of existing code, and
respects the fork's hard constraints: **no PSRAM** (fixed/bounded buffers), **no
WiFi/BLE** (SD + LoRa only), and **bounded CPU/heap**.

### P1 — Telemetry Probe: active repeater polling *(highest value)*

A new app (launcher entry `PROBE`) that reproduces the `mesh-metrics` core loop
on-device: walk a **watchlist** of repeater node IDs and, for each, send a
`TELEMETRY_APP` request with `want_response=true` to that `--dest`, honoring a
per-node dwell time, then present a live results table.

- **Columns:** node, Battery %, Voltage, chUtil %, airUtilTx %, uptime, temp/hum/pressure (if present), and a freshness/`up` indicator (responded this sweep / stale / no response) — a direct port of `meshtastic_up`.
- **Data source:** Plai already *receives and decodes* `DeviceMetrics`/`EnvironmentMetrics`; the only new primitive is emitting an outbound telemetry request to a specific dest (mirror of the existing own-telemetry broadcast in `mesh_service.cpp`, but unicast with `want_response`).
- **Reuse:** watchlist = existing favorites list; result rows reuse node-detail formatting already in `app_nodes.cpp`.
- **Why it matters:** turns Plai from a passive listener into an active field prober — a technician can walk a hill and confirm each repeater is answering **without** a laptop running `meshtastic --request-telemetry`.

### P2 — Health thresholds + on-device alerts

Port `meshtastic_rules.yml` into a small, config-driven threshold engine that
runs over telemetry Plai already sees (passively **and** via P1):

- Battery `< 30` warn / `< 15` crit; chUtil `> 60` warn / `> 70` crit (defaults match `mesh-metrics`, editable in Settings).
- Surface breaches via the **existing HAL** RGB LED and speaker alert tones, plus a badge on the node list. This is the field analogue of Alertmanager.
- **Reuse:** LED (`hal/led`), speaker alert tones (already per-channel), node-list badge rendering.

### P3 — Prometheus `.prom` / NDJSON export bridge *(ties the two projects together)*

Let Plai act as a **roaming collector** that feeds the `mesh-metrics` backend.
Add an SD exporter that writes the telemetry it gathers (P1 sweeps or passive)
in the **exact** `node_exporter` exposition format `mesh-metrics` uses:

```
meshtastic_Battery_level{node="!2f67c123"} 87
meshtastic_Total_channel_utilization{node="!2f67c123"} 22.5
meshtastic_up{node="!2f67c123",version="Plai"} 1
```

- **Output:** `/sdcard/metrics/meshtastic-<node>.prom` (per-node) or a single file, atomic-write, mirroring the daemon's options.
- **Value:** carry the SD card back to base and drop it straight into the `mesh-metrics` textfile-collector path — no re-instrumentation. Plai collects where the laptop can't go; `mesh-metrics` stores/graphs it. (NDJSON already exists via `MeshLogger`; this adds the Prometheus dialect.)

### P4 — Environment-metric history in Graphs

Extend the existing **Graphs** app (today: battery / channel activity / RSSI)
with **temperature, humidity, and barometric-pressure** history per node — the
three environment metrics `mesh-metrics` collects but Plai never trends.

- **Reuse:** `GraphPoint` history buffers and `LineGraph` widget already in `app_graphs` / `mesh_data.h`; add bounded per-node env series alongside `_rssi_history`.

### P5 — Channel-utilization / duty-cycle gauge

The single most actionable RF-health metric in `mesh-metrics` (chUtil / airUtilTx
with the 60/70 % crit thresholds) deserves a dedicated per-repeater gauge view
with color-coded bands, so a field operator can instantly spot a congested
channel — the on-device version of the Grafana utilization panel + alert rule.

### P6 — Availability / "up" timeline

Using P1's per-sweep `up` result, keep a bounded per-watched-node reachability
history (last-heard + a compact uptime bar) and flag nodes that went **dark**.
This is the field equivalent of `meshtastic_up` trended in Grafana — answering
"which repeater dropped, and when?" without a backend.

### P7 — Health coloring on the offline map

`mesh-metrics`' Grafana geomap turns alerting nodes **red**. Plai already has an
offline OSM map with node markers; color those markers by health (battery / util
/ staleness from P2) so the map doubles as a **coverage + health** view — the
standalone twin of the geomap, with zero extra dependencies.

### P8 — CSV roster interop (shared device list)

Adopt `mesh-metrics`' CSV schema (`NodeID,Contact,Location,Lat,Lon`) as an
import/export format for Plai's watchlist/favorites, so both tools share one
roster. Optionally read the **encrypted** list format (AES‑256‑CBC / PBKDF2,
OpenSSL `Salted__` header) that `meshmetricsd.py` already implements, for parity.

---

## 4. Suggested sequencing

1. **P1 (Telemetry Probe)** unlocks P2, P5, P6 (they consume its results) and is the biggest capability jump — do it first.
2. **P2 + P5** (thresholds & util gauge) are cheap once P1 lands and reuse existing LED/speaker/graph code.
3. **P3 (export bridge)** is the highest-leverage *integration* — it makes Plai and `mesh-metrics` a two-halves-of-one-system story (field collector ↔ backend).
4. **P4, P6, P7, P8** are incremental polish on top.

## 5. Constraints to respect

- **No PSRAM:** every new buffer must be fixed/bounded, like `node_threat.h`'s `ReplayTracker` and the SD-tail Log Reader. Per-node history series must be capped.
- **No WiFi/BLE:** the `mesh-metrics` Pushgateway/HTTP path is **not** portable; P3 uses **SD export** as the bridge instead.
- **Bounded CPU / airtime:** P1's active polling must honor a dwell time and hop limits so probing doesn't itself congest the channel (the same reason `mesh-metrics` documents increasing dwell time on larger networks).

---

*This document is a proposal only; no firmware behavior is changed by adding it.*
