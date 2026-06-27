# Fork Notes: Plai RadioDiag

This fork of **Plai** focuses on enhancing the diagnostic and network analysis capabilities of the M5Stack CardPuter when used as a standalone Meshtastic terminal.

## Key Changes & Additions

### 1. Enhanced Diagnostic Suite
Several new apps have been added to the launcher to help diagnose network health and node behavior:
- **MATRIX:** A heat-map grid for visualizing node recency and signal health.
- **RADAR:** Geographic bearing and live signal homing for specific nodes.
- **ROGUE:** Identification of noisy nodes based on airtime and packet rate.
- **GRAPHS:** Historical visualization of battery voltage and channel activity.
- **LOG READER:** Offline viewer for mesh logs stored on the SD card.

### 2. SD Mesh Logging
The fork includes a background logger that records mesh packets to the SD card in `.ndjson` format. This allows for long-term network analysis and offline review using the **Log Reader** app.
- Logs are stored in `/sdcard/logs/`.
- Format is machine-readable and includes timestamps, signal metrics, and decoded payload descriptions.

### 3. UI/UX Improvements
- **Expert Mode in Monitor/Log Reader:** Holding the `[CTRL]` key in packet lists reveals additional technical fields like absolute timestamps and inter-packet gaps.
- **Performance Optimizations:** Improved handling of large node databases and log files on the SD card.

## Documentation
Detailed operation manuals for the new diagnostic apps can be found in the [`/docs`](./docs/) folder.
