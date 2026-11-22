<p align="center">
  <img src="app/assets/nekodrop.png" width="128" alt="nekodrop">
</p>

<h1 align="center">nekodrop</h1>

<p align="center">
  <b>Native AirDrop on Windows with your laptop's built-in Wi-Fi card. Wi-Fi keeps working.</b><br>
  No USB dongle, no Linux VM, no Mac.
</p>

<p align="center">
  English · <a href="README.zh-CN.md">简体中文</a> · <a href="docs/COMPATIBILITY.md">Card Compatibility</a> · <a href="CHANGELOG.md">Changelog</a>
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078d4">
  <img alt="card" src="https://img.shields.io/badge/card-Intel%20AX201%20%7C%20AX200%20%7C%20AX210%20%7C%20AX211%20(user%20reports)-6c5ce7">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--2.0-blue">
  <img alt="status" src="https://img.shields.io/badge/status-experimental-orange">
</p>

<p align="center">
  <img src="docs/demo.gif" width="760" alt="Two-way AirDrop between iPhone and Windows 11 on built-in Intel AX201 while Wi-Fi stays connected">
  <br>
  <sub>
    <b>Real-hardware capture (unmodified iPhone ↔ Windows 11, built-in Intel AX201 on 5 GHz ch 44, Wi-Fi stays online):</b><br>
    ① iOS AirDrop discovers the PC and sends a photo → ② Accepted on Windows while WPA2 Wi-Fi stays connected → ③ Full-res (3556×2000) photo opens intact → ④ Explorer right-click → <i>Send with nekodrop</i> → ⑤ Pick nearby iPhone → ⑥ Native AirDrop banner opens in iOS Photos.
  </sub>
</p>

---

## What it does

- **Receive from iPhone, iPad and Mac.** nekodrop shows up in the AirDrop sheet like any Apple device. Files land in a folder you choose (default `Downloads\nekodrop`), and you can accept or decline each transfer from a notification.
- **Send to Apple devices.** Right-click a file in Explorer and choose **Send with nekodrop**. This is on the Windows 11 first-level menu, the classic menu and *Send to*. Then pick a nearby device.
- **Stay online.** In *Wi-Fi and AirDrop together* mode nekodrop drives the card itself. It joins your Wi-Fi on one virtual interface and runs AWDL (Apple Wireless Direct Link) on the other. Windows sees your internet connection as a wired adapter called **nekodrop**.
- **Manage Wi-Fi.** Scanning, connecting, hidden networks and saved networks with auto-connect all work. Saved Windows Wi-Fi profiles are imported, and captive portals (hotel, airport and campus sign-in pages) are detected and opened in your browser.
- **Recover by itself.** A service watches the radio, the AWDL daemon and the AirDrop engine and restarts whatever stops. It starts with Windows.
- **Tray app in Chinese and English**, with a light UI, system notifications and a settings window.

## Tested on real hardware

Everything below was tested by the author on their own laptop, not just in theory:

| | |
|---|---|
| Wi-Fi card | **Intel Wi-Fi 6 AX201 160MHz** (CNVi, firmware `iwlwifi-QuZ-a0-hr-b0-68`) |
| OS | Windows 11 Pro x64 |
| iPhone → Windows | ✅ works: photos and files arrive intact |
| Windows → iPhone | ✅ works: right-click → Send with nekodrop, accepted and received on the iPhone |
| Wi-Fi at the same time | ✅ works: the PC stays online through nekodrop's own WPA2 connection while AirDrop runs, including with Wi-Fi and AWDL on the same 5 GHz channel |
| Internet speed while AirDrop runs | about **28 Mbps** download at peak (≈2,370 packets/s sustained for 20 s, 5 GHz channel 44, 80 MHz, WPA2). Estimated from the radio's packet counters assuming full-size 1,500-byte packets, during ordinary use, not a speed test, so the link's real maximum may be higher. Block Ack declined (the default since 0.1.1); with Block Ack on, the peak seen was ~22 Mbps |

Other AX201 laptops should behave the same, since the chip and firmware are identical, but only this one has been tested. Reports from other machines are welcome in [Issues](../../issues).

## Supported hardware

nekodrop talks to the Wi-Fi chip directly through modular backends (`CardCatalog` + `radio_backend.h`). Today's built-in radios support Intel 22000/AX210-family cards (`nekodrop-radio.exe`, ported from [ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm)'s `hal_iwx`, concurrent Wi-Fi + AirDrop or on-demand) and **Realtek RTL8852CE** (`10EC:C852`, `nekodrop-radio-rtl.exe`, built from [winawdl](https://github.com/raph559/winawdl)/[devourer](https://github.com/raph559/devourer), on-demand mode — compiled and offline-tested in CI, awaiting real-hardware confirmation inside nekodrop). All 15 Intel cards keep 100% unchanged default concurrent and on-demand behavior.

**Full multi-vendor compatibility matrix (Intel / Realtek / MediaTek / Qualcomm): [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md)** · **Release history: [CHANGELOG.md](CHANGELOG.md)** · **Driver architecture: [docs/HARDWARE.md](docs/HARDWARE.md)**

Quick summary of currently active cards:

| Card | PCI IDs | Firmware / Backend | Status |
|---|---|---|---|
| **Intel Wi-Fi 6 AX201** 160 MHz (CNVi) | `8086:` `02F0` `06F0` `34F0` `3DF0` `4DF0` `43F0` `A0F0` | `Qu/QuZ-hr-b0-68` (`iwx`, Concurrent & On-Demand) | ✅ **tested** (QuZ on `43F0`) |
| Intel Wireless-AC 9560 / 9462 / 9461 on Qu/QuZ (CNVi) | same IDs as AX201 | `Qu/QuZ-jf-b0-68` (`iwx`, Concurrent & On-Demand) | ⚠️ untested |
| Intel Wi-Fi 6 AX200 (discrete) | `8086:2723` | `cc-a0-68` (`iwx`, Concurrent & On-Demand) | ✅ two-way AirDrop in user reports ([#1](../../issues/1), [#4](../../issues/4)) |
| Intel Wi-Fi 6E AX210 (discrete) | `8086:2725` | `ty-a0-gf-a0-68` + PNVM (`iwx`, Concurrent & On-Demand) | ✅ two-way AirDrop in user reports ([#14](../../issues/14), [#18](../../issues/18)); some machines still being fixed ([#11](../../issues/11)) |
| Intel Wi-Fi 6E AX211 (CNVi, 12th gen+) | `8086:` `7AF0` `7A70` `51F0` `51F1` `54F0` `7F70` | `so-a0-gf-a0-68` + PNVM (`iwx`, Concurrent & On-Demand) | ✅ two-way AirDrop in a user report ([#9](../../issues/9)); discovery not yet reliable on every machine ([#16](../../issues/16)) |
| Intel Wi-Fi 6E AX411 / Killer AX1690 (CNVi) | same IDs as AX211 | `so-a0-gf4-a0-68` + PNVM (`iwx`, Concurrent & On-Demand) | ⚠️ untested ([#7](../../issues/7)) |
| Intel Wi-Fi 6 AX203 / 9560 on So | same IDs as AX211 | `so-a0-hr/jf-b0-68` (`iwx`, Concurrent & On-Demand) | ⚠️ untested |
| **Realtek RTL8852CE** | `10EC:C852` | embedded (`devourer`, On-Demand only) | ⚠️ built-in, CI-compiled & offline-tested, untested inside nekodrop |
| Intel SnJ `2726`, QnJ `2720`, Meteor Lake `7E40`/`2729` | `8086:` | not shipped upstream | ❌ |
| Intel BE200 / BE201 (Wi-Fi 7) | `8086:` | no upstream driver | ❌ |
| Other Realtek, MediaTek, Qualcomm, Broadcom | | | ❌ |

To check your card: *Device Manager → Network adapters*. On a PC without a supported Wi-Fi card, the installer refuses and lists each detected wireless card, its PCI/USB ID, why nekodrop does not run on it, and how to run `installer/report.ps1`. On an untested card (including the built-in Realtek RTL8852CE backend), nekodrop may fail to start the radio; if it does, the card stays usable: run **restore** (below) and please report the log in an [issue](../../issues).

Cards are recognized by their PCI ID. When yours is not, *Settings > Wi-Fi card* lets you pick the card and its model by hand (for hardware IDs the driver package lists). To add a new card family to nekodrop, see [docs/ADDING-A-CARD.md](docs/ADDING-A-CARD.md).

Not sure which card you have, or want another card supported? The [diagnostic report](#reporting-a-problem-the-diagnostic-report) works without nekodrop installed: download [`installer/report.ps1`](installer/report.ps1) and run `powershell -ExecutionPolicy Bypass -File report.ps1`. It lists every wireless card with its PCI ID, chip, driver and why nekodrop does or does not run on it; please post it in an [issue](../../issues). On an installed nekodrop, `NekoDropService.exe --status` prints the matched card profile, capabilities and bound driver provider, and `NekoDropService.exe --selftest` runs offline catalog and radio-launch checks across all 16 active profiles (15 Intel + Realtek RTL8852CE) without a card or administrator rights. Intel's radio logs one `backend …` line at start and runs `iwx_selftest` on every build; the Realtek radio build runs its own offline unit tests (`block_ack_test`, `reorder_buffer_test`, `outgoing_session_test`, `dma_address_test`) on every build; `install.ps1 -SelfTest`, `restore.ps1 -SelfTest` and `report.ps1 -SelfTest` likewise run offline under Windows PowerShell 5.1 on every build. Intel cards keep 100% unchanged default concurrent and on-demand behavior. What other cards would take: [docs/HARDWARE.md](docs/HARDWARE.md).

### Something went wrong? One-click restore

Start menu → **nekodrop - restore Windows Wi-Fi** (or double-click `restore.cmd` in `C:\Program Files\nekodrop`). It is standalone (works even if nekodrop's install folder is damaged), stops nekodrop, gives the card back to its vendor driver without hardcoding Intel (keeping Intel rollback behaviour unchanged), removes nekodrop's driver packages and brings Windows Wi-Fi back, whatever state nekodrop left the card in. nekodrop stays installed; run the installer again to use it. Log: `C:\ProgramData\nekodrop\restore.log`.

### Reporting a problem: the diagnostic report

Logs are what let us fix things, especially on cards we don't have. Please attach them to every [issue](../../issues).

1. Start menu → **nekodrop - diagnostic report** (or tray icon → **Diagnostic report…**, or **Diagnostic report** at the bottom of the main window). Allow the administrator prompt; the logs and driver state are readable by administrators only.
2. Two files appear on your Desktop and the report opens in Notepad:
   - `nekodrop-report-<time>.md`: a summary, a step-by-step check of the last radio start (driver bind → firmware → PNVM → init → ready), system, card, driver, network and Bluetooth details, a discovery timeline (with how many seconds each step of discovery took) and the key log lines. It is also copied to the clipboard: **paste it into the issue**.
   - `nekodrop-logs-<time>.txt`: the full logs. **Drag it into the issue** as an attachment.
3. SSIDs, MAC and private IP addresses, your user name and the computer name are replaced by placeholders such as `<SSID-1>` and `<MAC-2>`. Nothing is uploaded. Please look the report over before posting.

**On an older nekodrop without this entry**, download [`installer/report.ps1`](installer/report.ps1) and run it from a command prompt: `powershell -ExecutionPolicy Bypass -File report.ps1`. Or collect the logs by hand:

| File | What's in it |
|---|---|
| `C:\ProgramData\nekodrop\service.log` (and `service.log.1`) | the service, the radio (firmware, AWDL, Wi-Fi) and awdld; readable by administrators |
| `%LocalAppData%\nekodrop\engine.log` (and `engine.log.old`) | the AirDrop engine: discovery, Bluetooth beacon, transfers |
| `C:\ProgramData\nekodrop\install.log` | the installer: driver, TAP adapter, firewall, service |
| `C:\ProgramData\nekodrop\restore.log` | the one-click restore |

## Requirements

- Windows 10 2004 (build 19041) or later, x64 (Windows 10 22H2 confirmed working in 0.1.16+, [#18](../../issues/18)).
- A supported Wi-Fi card: Intel Wi-Fi 6/6E (Concurrent & On-Demand) or Realtek RTL8852CE `10EC:C852` (built-in On-Demand, untested inside nekodrop; see above).
- Administrator rights to install.
- Bluetooth, for sending: AirDrop wakes nearby Apple devices with a Bluetooth LE beacon.
- On the Apple side, AirDrop set to **Everyone for 10 Minutes**. *Contacts Only* needs an Apple ID certificate nekodrop doesn't have.

## Install

1. Download `nekodrop-<version>-setup.exe` from [Releases](../../releases) and run it.
2. The installer:
   - copies the files to `C:\Program Files\nekodrop`;
   - creates a code-signing certificate **on your PC**, signs the small register-proxy driver with it, trusts it, and **deletes the private key**, so nothing else can ever be signed with it;
   - creates the TAP adapter **nekodrop** (TAP-Windows, signed by OpenVPN);
   - adds two inbound firewall rules, link-local addresses only (TCP 8771-8779, UDP 5353);
   - installs and starts the **NekoDropService** service.
3. The tray icon (the purple cat) appears. Open it and turn on **Allow receiving**.

On first start nekodrop takes the Wi-Fi card away from the Intel driver and reconnects you to the network Windows was using. The Windows Wi-Fi icon disappears; use nekodrop's **Wi-Fi…** window instead.

### Updates

nekodrop checks GitHub for a newer release a minute after it starts and then every 6 hours. When it finds one, a notification offers **Update**. nekodrop then downloads the installer, checks it against the SHA-256 GitHub publishes for it, and runs it (Windows asks for administrator rights). It never installs anything on its own. *Settings → Updates* turns the check off, chooses whether preview releases count, and has **Check now**. All releases are previews for now.

### Uninstall

Use *Settings → Apps → nekodrop → Uninstall*, or run `uninstall.ps1` in the install folder. Uninstalling gives the card back to the Intel driver, then removes the service, the driver package, the certificate trust and the firewall rules.

## Usage

| I want to… | Do this |
|---|---|
| Receive | Keep **Allow receiving** on. On the iPhone, tap Share → AirDrop and pick your PC's name |
| Send | Right-click files → **Send with nekodrop** → pick a device. Or drag files onto the main window |
| Change Wi-Fi | Tray → **Wi-Fi…**: pick a network, enter the password, Connect |
| Hidden network | **Wi-Fi…** → *Hidden network*: type the SSID and password |
| Sign in to hotel Wi-Fi | Connect; the sign-in page opens in your browser by itself |
| Give the card back to Windows | Tray → *Wi-Fi and AirDrop* → **Take the card on demand** or **AirDrop off** |
| Change name, folder or language | Tray → **Settings…** |

### Modes

| Mode | Internet | AirDrop |
|---|---|---|
| **Wi-Fi and AirDrop together** (default) | Through nekodrop's own Wi-Fi connection (adapter "nekodrop") | Always available |
| **Take the card on demand** | The normal Intel driver | nekodrop takes the card when an iPhone nearby opens AirDrop or you send. Internet drops during the transfer |
| **AirDrop off** | The normal Intel driver | Off |

### Wi-Fi security

| Network | Together mode |
|---|---|
| Open | ✅ |
| WPA/WPA2-Personal | ✅ |
| WPA2/WPA3 transition | ✅ (joins with WPA2) |
| WPA2 with protected management frames required, WPA2-PSK-SHA256 | 🧪 0.1.12, untested on hardware |
| Captive portal | ✅ detected, sign-in page opened |
| Hidden SSID | ✅ |
| WPA3-Personal only (SAE) | 🧪 0.1.12, untested on hardware (hash-to-element-only networks, mostly 6 GHz, not yet) |
| Enhanced Open (OWE) | 🧪 0.1.12, untested on hardware |
| WPA2/WPA3-Enterprise (802.1X, PEAP…) | ❌ not yet. nekodrop switches to *on demand* |
| WEP, WPA/TKIP | ❌ (insecure; not planned) |

### Experimental features

*Settings → Experimental*. Both are **off by default**, and with them off nekodrop behaves exactly as without them. Turning either on or off restarts AirDrop for a few seconds. If sending or discovery gets worse, turn it off, and please post a diagnostic report in an [issue](../../issues) either way.

(*Faster sending to iPhone*, a switch from 0.1.9, is always on since 0.1.25: up to 48 AWDL frames in flight instead of 8, one doorbell per batch, and the rate ladder starting at HT MCS 4. No card reported it making things worse: about the same 1.3 MB/s on an AX200 ([#4](../../issues/4)), a little faster on an AX210 ([#14](../../issues/14)), 537 MB at 3.4 MB/s with no frames given up on an AX211 ([#9](../../issues/9)).)

**Aggregated sending (A-MPDU)** (0.1.15+, **off by default**; on an AX200 and an AX211 the firmware has so far sent only single frames)

Sending to an iPhone is slow, about 1.3 MB/s, while receiving runs at about 9 MB/s. The reason: every frame we send goes out on its own and waits for its own acknowledgement, and only while the iPhone happens to be on our channel. The iPhone aggregates its frames, so receiving is fast. This switch ports ForbiddenItlwm's way of aggregating our side too (its `-itlawdlcoexbatx`, which reaches about 22 MB/s Mac → iPhone there):
1. When a large send to an iPhone starts (about 200 full-size frames a second), nekodrop gives that iPhone a firmware station of its own, with an aggregation queue and the firmware's rate control (VHT 80 MHz, two streams where the card has them).
2. It asks the iPhone for a Block Ack session (ADDBA), every 200 ms while it is on our channel, up to 40 times, then rests 20 s and tries again.
3. Once the iPhone agrees, the transfer's frames go out as QoS data on TID 0 and the firmware sends them as A-MPDUs. Short exchanges, such as a connection being set up, keep the ordinary path with its resends.
4. nekodrop follows the iPhone's AWDL schedule from its sync frames: which of its 16 slots it spends on our channel and when each starts. While the iPhone is away, its queue in the firmware is paused instead of burning retries, and other frames for it wait too, for at most the away time.
5. Promiscuous receive is off while the iPhone's station exists, so the firmware acknowledges the iPhone's frames.

What to check when testing:
- The diagnostic report gets an **A-MPDU sending** row: `agreed` means the iPhone accepted Block Ack.
- The service log gets an `aggregate:` line every 5 s. In it, *Block Acks* and *largest* say whether the firmware really sends aggregates (several frames per Block Ack), and *pauses* say whether the schedule pause works.
- If it says `asking` for a long time, the iPhone isn't answering our ADDBA. Upstream saw that too. Please attach the report.

Without enough memory for the extra ring, or on the kernel-driver path, the switch does nothing and sending stays as before.

**Faster unlock of a passive 5 GHz channel** (not yet tested on hardware)

Some cards refuse every country nekodrop asks for (an AX210 in Taiwan answered "status 4" from every source, [#11](../../issues/11)). The firmware then keeps the 5 GHz AirDrop channel *passive*: by the rules nothing may be sent there until an iPhone or a nearby router is heard on it, or until the firmware learns the country itself from routers' beacons. Until then the PC is silent, so iPhones cannot see it, and in #11 that took about a minute after every radio start. By default nekodrop runs one scan 10 s after start, over all 38 channels, to let the firmware hear routers. With this switch:
- the scan runs at 1.5 s, and again at 12 s and 30 s while the channel is still passive (the first waits up to 4 s while the Wi-Fi beside AirDrop is joining);
- it covers only the 11 common non-DFS channels (36–48, 149–161, 1, 6, 11), with the AirDrop channel's own block first, so a router there unlocks it straight away;
- the radio starts from the country the firmware really holds, not the one it was asked for, so a refused country is asked for again when an iPhone names it.

It never transmits on a passive channel: unlocking still waits for the firmware, a router or an iPhone. These scans don't replace the Wi-Fi list. On the kernel-driver path the switch does nothing.

What to check when testing: the diagnostic report's **Discovery timing** table shows, in seconds after the radio start, when the first AWDL frame was heard, when sending was allowed, when the channel turned active and when the AirDrop engine listed the first device. Compare it with the switch off.

## Status

| Part | State |
|---|---|
| User-mode Intel driver: firmware boot, monitor, inject, scan | ✅ works on AX201, AX200, AX210, AX211 (user reports for the last three) |
| iPhone → Windows | ✅ works (photos received intact, hash-checked) |
| Windows → iPhone | ✅ works (AX201; AX200, AX210, AX211 in user reports) |
| Wi-Fi and AirDrop at the same time | ✅ works (WPA2, DHCP, DNS through the "nekodrop" adapter) |
| Wi-Fi manager, saved networks, captive portal | ✅ |
| Service, tray app, notifications, context menu, installer | ✅ |
| WPA3-only, OWE | 🧪 0.1.12, untested on hardware |
| 802.1X (Enterprise) | ⏳ planned |
| AX200, AX210, AX211 | ✅ two-way AirDrop in user reports |
| AX411, 9560 on Qu/So, AX203 | 🧪 ported from upstream, untested on hardware |
| Windows 10 | ✅ two-way AirDrop confirmed in user reports on 0.1.16+ ([#18](../../issues/18)) |
| Kernel WiFiCx driver (`kdriver/`) | 🧊 shelved: experimental and **not installed** by the setup. See [docs/KERNEL.md](docs/KERNEL.md) |

## How it works

```
 nekodrop.exe (tray app, your session) ──JSON pipe──► NekoDropService (SYSTEM)
   │  AirDrop engine (Python)                         │ takes/gives back the card
   │  notifications · context menu · send window      │ Wi-Fi manager, watchdogs
   │                                                  ▼
   └── awdl0 (Wintun, IPv6) ◄── awdld (AWDL, OWL) ◄─pipe─ nekodrop-radio (user-mode Intel driver)
                                                          ├ MAC 0: AWDL on channel 6/44/149
     "nekodrop" TAP adapter ◄── Ethernet bridge ◄─────────┤ MAC 1: station, joined to your router
     (Windows' internet)                                  └ UMDF register proxy + user-mode DMA → AX201
```

1. **Register proxy.** A tiny UMDF driver (`driver/regproxy`) binds to the AX201 in place of Intel's driver. It does nothing but map the card's registers and interrupts into one user-mode process. No kernel code of ours runs.
2. **User-mode driver.** `nekodrop-radio` (`iwxradio/`) is a port of the OpenBSD/itlwm `iwx` driver to user mode. It loads Intel's firmware through user-mode DMA, then runs two MACs at once: one listens and injects AWDL frames, the other is a normal Wi-Fi station with WPA2 (4-way handshake, keys in firmware, rate control).
3. **AWDL.** `awdld` (OWL, the Open Wireless Link) speaks Apple's AWDL protocol over the radio pipe and exposes the link as the IPv6 interface `awdl0`.
4. **AirDrop.** A Python engine speaks AirDrop over `awdl0`: mDNS discovery, HTTPS on port 8771 (spare ports 8772-8779), the `/Discover`, `/Ask` and `/Upload` flow, and the BLE beacon used for sending.
5. **Internet.** Frames from the station MAC are bridged to the TAP adapter "nekodrop". Windows treats it as Ethernet and gets an address by DHCP.

Design notes: [docs/DESIGN.md](docs/DESIGN.md) (driver, DMA, risks) · [docs/CLIENT.md](docs/CLIENT.md) (service, client, modes)  · [docs/TESTING.md](docs/TESTING.md) (throughput test method)


## Troubleshooting

| Symptom | Try |
|---|---|
| iPhone doesn't see the PC | Set the iPhone's AirDrop to *Everyone for 10 Minutes*, turn it off and on (this clears a cached record), and keep the phone unlocked nearby |
| The PC shows up twice on the iPhone | A cached record from an earlier run. Toggle AirDrop on the iPhone |
| No internet | Tray → **Wi-Fi…**: check you're connected. The network may need a sign-in page or be WPA3-only/802.1X (see above) |
| The Windows Wi-Fi icon disappeared | Expected in *together* mode: nekodrop owns the card. Use **Wi-Fi…**, or switch to *on demand* |
| Something else | Open an [issue](../../issues) with the [diagnostic report](#reporting-a-problem-the-diagnostic-report) |

## Building from source

Needs Visual Studio 2022 Build Tools (C++, Windows SDK, CMake, vcpkg), the .NET 8 SDK and Git. Inno Setup 6 is optional (without it you get a zip). No WDK install is needed: the NuGet package is used.

```powershell
git clone https://github.com/zheznanohana/nekodrop.git
cd nekodrop
.\scripts\fetch-deps.ps1        # winawdl, devourer, ForbiddenItlwm (firmware), MinGW, WDK NuGet → third_party\
.\scripts\build-installer.ps1   # everything → build\nekodrop-0.1.8-setup.exe
.\scripts\build-radio.ps1       # just the Intel user-mode driver, plus its offline self-tests
.\scripts\build-radio-rtl.ps1   # just the Realtek RTL8852CE radio (devourer), plus its offline unit tests
```

| Folder | What |
|---|---|
| `iwxradio/` | User-mode Intel driver (C++): firmware, AWDL monitor/inject, station, WPA2, UMAC scan |
| `driver/regproxy/` | UMDF register-proxy driver |
| `app/service/` | NekoDropService (C#): card ownership, radio supervision, Wi-Fi manager |
| `app/client/` | Tray app (C# WPF): UI, notifications, AirDrop engine host |
| `app/shellext/` | Windows 11 context-menu handler |
| `installer/` | Inno Setup script, install/uninstall scripts, TAP setup |
| `kdriver/` | Shelved kernel WiFiCx driver (experimental, not shipped) |

## Where the code comes from

nekodrop is built on other open-source projects. This section says exactly what was **used as source code** (copied, ported or bundled) and what was only **studied as a reference**.

### Source code used

| Project | License | What nekodrop takes from it | Where it ends up |
|---|---|---|---|
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) 0.6.0 (itlwm fork, macOS) | GPL-2.0 | The Intel 22000-series driver `hal_iwx`: firmware loading, TLV parsing, context info, TX/RX rings, firmware commands, the register header `if_iwxreg.h`. Its AWDL strategies too: channel following (the 44/149 min rule, country-code learning), receive Block Ack, promiscuous off during BA, rate stepping, host-side unicast resend, beacon hints | `iwxradio/`, ported from kernel C to a user-mode C++ driver |
| ForbiddenItlwm's firmware folder | Intel redistribution license | iwlwifi firmware `iwlwifi-Qu*-hr-b0-68.ucode` | shipped in `radio\firmware\` |
| [raph559/winawdl](https://github.com/raph559/winawdl) | MIT / GPL-2.0 (`radio/`) | The way to take over a PCIe Wi-Fi card from user mode on Windows: the UMDF register-proxy driver, user-mode DMA, the radio pipe protocol, and the RTL8852CE radio daemon (`radio/`) | `driver/regproxy/` (Intel + RTL8852CE PCI IDs), `iwxradio/` (pipe and DMA), `radio\nekodrop-radio-rtl.exe` |
| [raph559/devourer](https://github.com/raph559/devourer) (`winawdl/pcie-windows`) | GPL-2.0 | Realtek RTL8852CE user-mode PCIe driver (`KESTREL_8852C`, embedded firmware and A-MPDU stack) | built into `radio\nekodrop-radio-rtl.exe` (`licenses\devourer-GPL-2.0.txt`) |
| winawdl `awdl/` (`awdld`) | GPL-3.0-or-later (builds on OWL) | The AWDL daemon: OWL's protocol core with a Windows I/O layer and Wintun | built from source, shipped as `awdl\awdld.exe` |
| winawdl `airdrop/` | MIT | The AirDrop engine in Python: mDNS, the HTTPS receiver, the sender, the BLE beacon | shipped as `airdrop\` with two small patches (`scripts/patch-engine.py`: stable instance name, spare ports) |
| [seemoo-lab/owl](https://github.com/seemoo-lab/owl) | GPL-3.0 | The AWDL implementation inside `awdld` | through winawdl |
| [Wintun](https://www.wintun.net/) | Wintun license | The `awdl0` virtual interface | `awdl\wintun.dll` (prebuilt) |
| [TAP-Windows6](https://github.com/OpenVPN/tap-windows6) | GPL-2.0 / OpenVPN | The "nekodrop" virtual Ethernet adapter for Windows' internet | installed by the setup (signed OpenVPN installer) |

### Studied as references (no code copied)

| Project | What it was used for |
|---|---|
| OpenBSD `iwx(4)`, Linux `iwlwifi` | How the firmware, UMAC scan, station mode and keys work |
| [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop) | The AirDrop protocol (`/Discover`, `/Ask`, `/Upload`, mDNS records) |
| [microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) | WiFiCx structure for the shelved kernel driver (`kdriver/`) |
| [t4t5/owly](https://github.com/t4t5/owly), [JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | Other AirDrop-on-other-platforms attempts, compared during design |

### Written for nekodrop

- `iwxradio/`: the user-mode port itself, plus everything Windows-specific: running two MACs at once (AWDL listener plus Wi-Fi station on its own PHY), station mode with WPA2 (4-way handshake, keys in firmware, TLC rate control), bridging to TAP, firmware UMAC scan, beacon parsing and security detection.
- `app/service/`: the Windows service: card ownership, radio and awdld supervision, the Wi-Fi manager (DPAPI-encrypted saved networks, auto-connect, hidden networks, captive-portal detection).
- `app/client/`: the tray app (WPF), notifications, the send window, the Wi-Fi window, Chinese/English UI.
- `app/shellext/`: the Windows 11 context-menu handler.
- `installer/`: the Inno Setup installer, the on-PC driver signing, TAP setup and uninstall.

## Contributors

Thanks to everyone who sent code and careful test reports:

- [@BaiBaiAi](https://github.com/BaiBaiAi): Windows → iPhone sending without a client certificate ([#13](../../pull/13)), pipelined AWDL transmit so large uploads finish ([#15](../../pull/15)), and the first AX210 test report ([#14](../../issues/14)).
- [@XXD051030](https://github.com/XXD051030): the optional balanced Bluetooth coexistence mode for Intel cards, so the PC finds iPhones and Macs in seconds instead of minutes, with a careful before/after study on an AX200 ([#26](../../pull/26)).

## License

nekodrop as a whole is **GPL-2.0** (see [LICENSE](LICENSE)), because `iwxradio/` derives from itlwm/ForbiddenItlwm.

- `if_iwxreg.h`: dual BSD/GPL-2.0.
- `driver/regproxy` and the AirDrop engine: keep winawdl's MIT license.
- `nekodrop-radio-rtl.exe` (`raph559/winawdl` `radio/` + `raph559/devourer`): GPL-2.0 (`licenses\devourer-GPL-2.0.txt`).
- `awdld`: GPL-3.0-or-later, from winawdl/OWL. It is a separate program talking over a pipe.
- iwlwifi firmware: Intel's redistribution license (`licenses\iwlwifi-firmware.txt`).
- Wintun and TAP-Windows: their own licenses.

The installer copies every license into `licenses\`.

## Community

- [LINUX DO](https://linux.do/) — friendly community link

## Disclaimer

nekodrop is an independent project, not affiliated with or endorsed by Apple or Intel. AirDrop, iPhone, iPad and Mac are trademarks of Apple Inc.; Intel is a trademark of Intel Corporation. nekodrop replaces your Wi-Fi driver while it runs. It's experimental software, provided as is, without warranty.
