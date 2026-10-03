# SmartWorks ESP32-P4 Carton Packing Terminal

Standalone ESP-IDF project for the USB QR-reader and LVGL packing-room display.
The repository contains the application, fonts, logo, board defaults, dependency
lockfile, local network-setup page, and desktop regression checks. ESP-IDF's
component manager downloads the declared third-party components during a build.

Display-only carton packing terminal for the Waveshare ESP32-P4-Module-DEV-KIT:

- Waveshare 10.1-inch DSI Touch A panel, used as a display only (800x1280 native)
- USB Type-A port in HOST mode with a HID keyboard-style QR scanner
- ESP32-C6 coprocessor over SDIO for 2.4 GHz Wi-Fi and BLE
- On-board IP101 Ethernet PHY with DHCP (preferred uplink)
- Automatic packed-carton confirmation through the warehouse gateway
- No touch or buttons for packing; phone/laptop browser for network setup

## Ethernet and Wi-Fi setup

Connect the board's RJ45 port to a **router/LAN with DHCP**, not an EtherCAT drive.
Ethernet and Wi-Fi can be connected together; Ethernet has routing priority.
Removing its cable lets new requests use Wi-Fi. An in-flight carton transfer can
still time out during a switch and is never automatically submitted a second time.
This is link/DHCP failover, not an internet-health check: a connected Ethernet
network without internet will still be preferred.

While online, the bottom of the display shows only Ethernet/Wi-Fi status, the
board's HTTP address(es), and an eight-digit **SETUP CODE**, without setup
instructions. Hotspot instructions appear only when both uplinks are offline.
On a phone or computer on that same LAN:

1. Open the displayed `http://<IP-address>` (use HTTP, not HTTPS).
2. Enter the 2.4 GHz Wi-Fi SSID, password, and displayed setup code.
3. Press **Connect & save**. Allow up to 45 seconds for association and DHCP.
4. Settings are saved in the board's NVS only after DHCP succeeds and survive
   power-off. A failed attempt restores the previously saved network.

If neither Ethernet nor Wi-Fi has an address for 20 seconds, the board starts
**Rearly-Setup-XXXX**. Join that Wi-Fi using the eight-digit **KEY** on the display,
then manually open **http://192.168.4.1**. The key also serves as the web setup code.
Accept your phone's “stay connected without internet” prompt. This is a protected
setup hotspot, not an internet-sharing router or an automatic captive-portal popup.
The hotspot also starts before testing a new network so a wrong password does not
lock you out. It stops after an uplink has been stable for two minutes and no
clients remain connected. The web page stays available at the regular LAN address.

Up to **five known Wi-Fi networks** are kept in NVS. Factory networks can be set
in the private `main/secrets.h`; this local installation uses **RTUniverse** and
**NILA EDP**. Their passwords and this private file are excluded from Git.
The setup page lists saved SSIDs but never passwords. Adding a validated network
makes it the first choice; existing SSIDs have their password updated without
creating duplicates. Other networks are retained, and a full list rejects new
SSIDs rather than silently evicting one. When Wi-Fi is unavailable, firmware tries
the next saved profile every 20 seconds while Ethernet remains independent.
The former single-network record is migrated automatically. Built-in profiles are
seeded only if absent, so later browser password updates are not overwritten.
If a saved password is wrong, use the setup hotspot, enter that **same SSID** with
the corrected password, and press **Connect & save**. The entry is updated only
after successful DHCP; unsuccessful attempts do not overwrite it. Form fields are
not changed by background network cycling once the user starts editing.
SSIDs are 1–32 UTF-8 bytes; passwords are 8–63
printable ASCII characters, or empty for an open network. Enterprise/captive-portal
networks and raw 64-hex-digit PSKs are not supported. The setup AP uses
192.168.4.0/24; avoid that same subnet on the upstream LAN while provisioning.

Use the setup page on trusted networks only: HTTP does not encrypt browser traffic.
The hotspot uses WPA2; LAN changes require the displayed code, with a 30-second
lockout after five incorrect codes. Passwords/codes are not returned by status APIs
or logged. NVS is not encrypted in this development build; physical flash access
can reveal stored credentials. Factory-erasing NVS clears network settings and
regenerates the setup code (and also clears packing-session persistence).

## Workflow

1. The scanner types the printed 24-character carton EPC QR value and terminates it with Enter.
2. The display shows an animated QR-check sequence; rapid scans continue filling the 64-entry queue.
3. Firmware refreshes and caches the available carton IDs from **Labels-V**:

   `GET /wms/labels`

4. The Supervisor packing lookup supplies the carton/session metadata shown in the result:

   `GET /api/v3/marine/packing/track-a/cartons/{cartonNumber}`

5. Firmware uses the same Ante Room route and payload as the working Android application:

   ```json
   {"rfid_tag":"<qr>","item_code":"<Labels-V item_code>"}
   ```

   `POST /packing/ante-stock-in`

6. The warehouse service verifies that the carton is available in **Labels-V**.
7. If valid, it moves the carton to **Stores-V (Ante-Room)**, confirming that
   packing is complete.
8. A successful result shows the carton moving into **Ante-Room**, a green check,
   product, grade, blocks, net weight, session reference, and trace. After five
   seconds it returns to the current packing status. A new scan can interrupt
   that result; it cannot replace the carton currently being processed.

## Packing activity display

The home screen polls the current Load Note every 30 seconds, using the read-only
route from the PackingSessionDisplay example:

`GET /api/v3/marine/packing/load-notes/current?stationId=packing-station-01`

- **No packing planned:** empty stationary conveyor and a quiet status beacon.
- **Packing planned:** staged cartons, amber state, active varieties and grades.
- **Packing on:** moving conveyor and cartons with green state. This begins after
  the first successful Ante Room scan. The Load Note reference is saved in NVS so
  the state survives a reboot until the active Load Note changes or ends.
- **QR check:** cyan scanner sweep and activity ring, followed by transfer animation.
- **Result:** green check and delivery, red cross and a short carton shake, or an
  amber duplicate symbol. A shrinking line shows the time until the home screen returns.

Variety cards show grade, expected blocks, prepared/expected labels, grade state,
and a progress bar. Three cards appear per page; additional cached grades rotate
automatically every seven seconds. The packed/rejected/queued/available counter
tiles are intentionally omitted. Operator-facing warehouse captions are **Carton**
and **Ante-Room**; backend Labels-V / Stores-V identifiers remain unchanged.
Wi-Fi, Ethernet, BLE, QR status dots, network setup address,
and the Rearly logo remain visible.

Graphics are LVGL vector objects updated at about 15 frames/second. Hidden scenes
skip their animation updates. Short slide transitions avoid full-panel fades.
The Noto Sans bold fonts use uncompressed bitmaps.

The display uses only carton and warehouse terminology. Backend implementation details
and administrative credentials are deliberately not exposed to operators or stored in
the firmware.

## Fast scanning behaviour

- Up to 64 cartons can wait in the local processing queue.
- Accidental repeats of the same label within three seconds are ignored.
- Each accepted carton is processed in scan order.
- Newly queued or duplicate scans do not overwrite an in-flight carton result.
- A QR is announced as queued only after the processing queue accepts it. Late
  queue notifications for a scan that has already started or completed are ignored.
- Processing and final-result updates wait for the LVGL display lock instead of
  being silently dropped after 250 ms. The complete **PACKED / ANTE-ROOM** screen
  and its five-second return timer are committed together.
- Waiting cartons take priority over the periodic Load Note refresh. Serial logs
  include scan IDs, queue-wait time, stock-in outcome, and the committed success
  screen so backend and display progress can be correlated.
- A timeout is never retried automatically because the transfer result may be unknown.
- When both Ethernet and Wi-Fi are offline, the carton is not moved and the operator is told to scan it
  again after connectivity returns.

## Hardware

1. Connect the 10.1-inch DSI-A panel before powering the board.
2. Put the Type-A USB selector/jumper in **HOST** position.
3. Connect the QR scanner to the Type-A port.
4. Connect the board's USB-UART/programming connector to the computer.

The scanner must be a 2D QR reader that presents a USB HID keyboard interface and
emits Enter after each scan.

## Build and flash

Use **ESP-IDF v6.0.2** with the `esp32p4` toolchain installed, Git, CMake, and Ninja.
The tested board has ESP32-P4 **rev 1.0**, a 16 MB flash, a 10.1-inch DSI-A
800x1280 display, and factory ESP-Hosted 3.0.x firmware on its C6 coprocessor.
The checked-in defaults target pre-v3 P4 silicon and do not flash the C6.

```bash
git clone git@github.com:rearlyapp/p4-qr-display.git
cd p4-qr-display
cp main/secrets.example.h main/secrets.h
```

Edit `main/secrets.h` for your Wi-Fi networks and warehouse service addresses.
`STOCKIN_EXTRA_WIFI_SSID` and `STOCKIN_EXTRA_WIFI_PASSWORD` are optional. The
network setup page can also save Wi-Fi networks after Ethernet is connected or
the fallback hotspot starts. No ERP API key or secret is required by this flow.

Activate your installed SDK, then build and flash:

```bash
source /path/to/esp-idf/export.sh
./idf.sh set-target esp32p4
./idf.sh build
./idf.sh -p /dev/ttyACM0 flash
./idf.sh -p /dev/ttyACM0 monitor
```

Replace the SDK path and serial port with those on your computer. `idf.sh` uses
the active SDK or `IDF_PATH`; it does not depend on this developer's workspace.
On Windows, activate the ESP-IDF command prompt and use the equivalent
`idf.py set-target esp32p4`, `idf.py build`, and `idf.py -p COMx flash monitor`.
The committed `dependencies.lock` pins the tested component versions;
`sdkconfig.defaults` provides the board configuration for a fresh checkout.
Exit the serial monitor with **Ctrl+]**.

`main/secrets.h`, generated SDK configuration, downloaded components, build
directories, and firmware binaries are git-ignored. Build your own binary after
configuring credentials: a firmware binary can contain those configured secrets.

Expected boot milestones include:

- `PASS: P4-to-C6 ESP-Hosted link ready`
- `PASS: Ethernet started` (an IP is logged separately when a DHCP LAN is connected)
- `PASS: Wi-Fi DHCP IP=...` / `PASS: Ethernet DHCP IP=...`
- `PASS: local network setup server ready on port 80`
- `PASS: BLE enabled and advertising as SmartWorks-Cartons`
- `PASS: USB HID QR scanner ready`

## Render and check the UI on a laptop

This uses the same LVGL UI and fonts as the firmware, with synthetic packing data.
It performs no network or inventory operations. After the managed LVGL component
has been downloaded by the normal firmware build:

```bash
cmake -S preview -B build-ui-preview -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ui-preview -j 6
./build-ui-preview/packing_ui_preview build-ui-preview
```

The preview generates PNGs for idle, planned, running, checking, transferring,
accepted, rejected and duplicate states. It also checks rapid-scan ownership,
automatic return timers, return to Packing On, and the unknown-result timeout state.
The regression checks inject slow-frame lock timeouts and late queue notifications
across 100 UI-only check-ins, without changing warehouse data.
It also renders the network setup footer. Credential validation can be tested without
network access or hardware:

```bash
cc -std=c11 -Wall -Wextra -Werror preview/test_network_credentials.c -o build-ui-preview/test_network_credentials
./build-ui-preview/test_network_credentials
```

`node preview/test_network_http.mjs http://<board-IP>` checks the live setup page,
malformed requests, status privacy and incorrect-code rejection without submitting
valid Wi-Fi credentials or calling any inventory endpoint. It briefly triggers the
30-second setup-code rate limit. Do not run it during a real provisioning session.

For an **explicit provisioning test**, with both uplinks connected and the active
SSID matching `main/secrets.h`, run
`node preview/test_wifi_provisioning.mjs http://<Ethernet-IP>` and enter the display
code at its hidden prompt. This re-saves the existing Wi-Fi credentials, tries a
nonexistent test network, and verifies timeout/rollback while Ethernet stays up.
It changes Wi-Fi temporarily but performs no inventory operations. Do not unplug
Ethernet while this test is running.

If the scanner stays offline, verify the Type-A connector's HOST selector and that the
scanner is a HID-keyboard model. If the panel remains dark or shows incorrect geometry,
confirm it is the Waveshare 10.1-inch DSI Touch A, not the B variant.

The bundled `ui_bold_*.c` fonts were converted from Noto Sans Bold. Their copyright
notice and SIL Open Font License 1.1 are included in `main/fonts/OFL.txt`.
