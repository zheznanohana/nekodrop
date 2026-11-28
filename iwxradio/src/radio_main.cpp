// SPDX-License-Identifier: GPL-2.0-only
/*
 * iwx_radio: the radio daemon for Intel AX201-class cards.
 *
 * It speaks winawdl's radio link (radio_link.h) on \\.\pipe\winawdl-radio,
 * so winawdl's AWDL daemon (awdld, OWL on Windows), its AirDrop engine and its
 * app run on top unchanged; only the Realtek radio (devourer) is replaced by
 * the user-mode Intel driver in iwx.cpp. Built as winawdl_radio.exe it drops
 * into winawdl's layout.
 *
 * Usage: winawdl_radio <channel> [--width 20|40|80] [--country XX]
 *          [--mac-type listener|ibss|sta] [--firmware DIR]
 *          [--data-rate 6|12|24|36|48|54|MCS0..MCS15] [--verbose]
 *          [--tx-window N] [--aggregate] [--fast-unlock] [--coex-plan] [--family qu|ax200|ax210|so|be200]
 *          [--bt-coex 1|2|3] [--sta-yield] [--sta-yield-short] [--bz-block-ack]
 * winawdl's own options (--retries, --capture, --no-block-ack,
 * --no-aggregate, --no-quick-submit) are accepted and ignored.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "awdl_policy.h"
#include "coex_plan.h"
#include "sta_yield.h"
#include "awdl_schedule.h"
#include "iwx.h"
#include "wifi_sta.h"
#include "pipe_server.h"
#include "kdev.h"
#include "radio_backend.h"

#include <optional>
#include <type_traits>
#include <map>
#include "radio_link.h"

/* allow_peer_channel: NEKODROP_PIN_CHANNEL=<n> keeps the service on channel n
 * even when a peer advertises a different channel sequence.  A macOS peer
 * publishes its station channel (often 5 GHz) in the sequence while its AWDL
 * listener stays on the coexist channel, so following the sequence blindly
 * moves the radio away from the only channel that peer can actually hear. */
static bool allow_peer_channel(unsigned channel) {
  static const unsigned pin = [] {
    const char *value = std::getenv("NEKODROP_PIN_CHANNEL");
    const int parsed = value ? std::atoi(value) : 0;
    return parsed > 0 ? static_cast<unsigned>(parsed) : 0u;
  }();
  return pin == 0 || pin == channel;
}

namespace {

std::atomic<bool> g_stop{false};

bool stopping() { return g_stop.load(); }

BOOL WINAPI on_console(DWORD) {
  g_stop = true;
  return TRUE;
}

void log_line(const std::string &line) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  std::fprintf(stderr, "%02u:%02u:%02u.%03u radio: %s\n", t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds, line.c_str());
  std::fflush(stderr);
}

struct Options {
  uint8_t channel = 0;
  unsigned width = 0; /* 0: 80 on 5 GHz, 20 on 2.4 GHz, as winawdl */
  RadioOpenOptions dev; /* any backend's open() takes these */
  TxRate data_rate{TxRate::Legacy, 48, 0}; /* 24 Mb/s */
  bool fixed_rate = false;  /* --data-rate given: no rate ladder */
  bool no_block_ack = false; /* decline every peer's receive Block Ack */
  /* --bz-block-ack: accept receive Block Ack on Bz cards (declined by default). */
  bool bz_block_ack = false;
  std::string sta_config; /* --sta-config: join this network beside AWDL */
  /* Test mode: serve the pipe without touching the card, and report what the
   * client sends. Checks the radio link against awdld with no hardware. */
  bool no_card = false;
  /* --kernel: the card runs nekodrop-wlan.sys (Windows keeps its Wi-Fi);
   * AWDL goes through the driver's NekodropAwdl interface. */
  bool kernel = false;
  /* --pcap FILE: every received frame, with radiotap, for Wireshark. */
  std::string pcap;
  /* --tx-window N (experimental "faster sending"): AWDL unicast frames kept in
   * flight, 8 by default; above 8 the doorbell is also rung once per batch. */
  unsigned tx_window = 8;
  /* --aggregate (experimental "A-MPDU sending"): Block Ack and firmware
   * aggregation to the iPhone, with ForbiddenItlwm's peer-schedule pause. */
  bool aggregate = false;
  /* --he80 (experimental "high-throughput mode"): Wi-Fi 6 HE80 TX TLC +
   * 8 KiB RX DMA buffers + ADDBA RX A-MSDU negotiation. */
  bool he80 = false;
  /* --fast-unlock (experimental "fast passive-channel unlock", #11): on a
   * passive 5 GHz AWDL channel, discovery scans at 1.5, 12 and 30 s over the
   * non-DFS channels instead of the one full scan at 10 s, and the country
   * the firmware really holds as the starting point (radio_backend.h). Off:
   * the radio behaves exactly as before. */
  bool fast_unlock = false;
  /* --coex-plan (experimental, ForbiddenItlwm PR #30): with the station on
   * another 5 GHz channel than AWDL, name the router in some of our 16
   * entries, stay silent and hold aggregate sending in them (coex_plan.h). */
  bool coex_plan = false;
  /* --sta-yield: pause the station link during AWDL transfers (sta_yield.h).
   * On by default: with the station on another channel the firmware keeps
   * leaving the AWDL channel and the iPhone's unicast frames are lost, which
   * drops a transfer from 3 MB/s to KB/s (hardware, AX203, 2026-11-27).
   * --no-sta-yield turns it off. */
  bool sta_yield = true; /* AX203 only: other cards never pause the station */
  /* A pause lasts until AWDL is idle for 30 s (at most 120 s between visits
   * to the access point), so it covers a whole AirDrop session: ask, the
   * user's accept, upload. --sta-yield-short: 5 s idle, 20 s pauses. */
  bool sta_yield_hold = false; /* review of #95: short pauses by default, --sta-yield-hold for long */
};

bool parse_rate(const std::string &s, TxRate &rate) {
  if (s.size() > 3 && (s[0] == 'M' || s[0] == 'm') && (s[1] == 'C' || s[1] == 'c')) {
    rate.kind = TxRate::Ht;
    rate.mcs = static_cast<unsigned>(std::atoi(s.c_str() + 3));
    return rate.mcs <= 15;
  }
  const double mbps = std::atof(s.c_str());
  if (mbps <= 0) return false; /* "auto", "client": keep the default */
  rate.kind = TxRate::Legacy;
  rate.mbps_x2 = static_cast<unsigned>(mbps * 2 + 0.5);
  return true;
}

std::string exe_dir() {
  char path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, path, MAX_PATH);
  std::string s = path;
  const size_t slash = s.find_last_of("\\/");
  return slash == std::string::npos ? "." : s.substr(0, slash);
}

bool parse_options(int argc, char **argv, Options &o) {
  o.dev.firmware_dir = exe_dir() + "\\firmware";
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--width") o.width = static_cast<unsigned>(std::atoi(value().c_str()));
    else if (a == "--country") o.dev.country = value();
    else if (a == "--firmware") o.dev.firmware_dir = value();
    else if (a == "--mac-type") {
      const std::string t = value();
      if (t == "listener") o.dev.mac_type = RadioMacType::Listener;
      else if (t == "ibss") o.dev.mac_type = RadioMacType::Ibss;
      else if (t == "sta") o.dev.mac_type = RadioMacType::Sta;
      else return false;
    } else if (a == "--data-rate") o.fixed_rate = parse_rate(value(), o.data_rate);
    else if (a == "--verbose") o.dev.verbose = true;
    else if (a == "--no-card") o.no_card = true;
    else if (a == "--kernel") o.kernel = true;
    else if (a == "--pcap") o.pcap = value();
    else if (a == "--sta-config") o.sta_config = value();
    else if (a == "--retries" || a == "--capture") value();
    else if (a == "--no-block-ack") o.no_block_ack = true;
    else if (a == "--bz-block-ack") o.bz_block_ack = true;
    else if (a == "--aggregate") o.aggregate = o.dev.aggregate = true;
    else if (a == "--he80") o.he80 = o.dev.he80 = true;
    else if (a == "--fast-unlock") o.fast_unlock = true;
    else if (a == "--coex-plan") o.coex_plan = true;
    else if (a == "--no-sta-yield") o.sta_yield = false;
    else if (a == "--sta-yield") o.sta_yield = true;
    else if (a == "--sta-yield-short") o.sta_yield_hold = false;
    else if (a == "--sta-yield-hold") o.sta_yield_hold = true;
    else if (a == "--family") {
      o.dev.family_device = cards::family_device(value());
      if (!o.dev.family_device) return false;
    }
    else if (a == "--tx-window") {
      const int n = std::atoi(value().c_str());
      if (n < 1 || n > 64) return false;
      o.tx_window = static_cast<unsigned>(n);
    }
    else if (a == "--bt-coex") {
      const int n = std::atoi(value().c_str());
      if (n < 1 || n > 3) return false;
      o.dev.bt_coex = static_cast<uint32_t>(n);
    }
    else if (a == "--no-aggregate" || a == "--no-quick-submit") {}
    else if (!a.empty() && std::isdigit(static_cast<unsigned char>(a[0])) && !o.channel)
      o.channel = static_cast<uint8_t>(std::atoi(a.c_str()));
    else return false;
  }
  if (!o.channel) return false;
  if (!o.width) o.width = o.channel <= 14 ? 20 : 80;
  if (o.width != 20 && o.width != 40 && o.width != 80) return false;
  if (o.dev.country.size() != 2) return false;
  for (auto &c : o.dev.country) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return true;
}

/* The fields of a radiotap header this daemon reads: total length, the rate
 * and the MCS index if given. Walks only the standard fields up to MCS. */
/* A pcap file of 802.11 frames with radiotap headers (link type 127). */
class PcapWriter {
public:
  bool open(const std::string &path) {
    _f = std::fopen(path.c_str(), "wb");
    if (!_f) return false;
    const uint32_t magic = 0xa1b2c3d4, snap = 65535, link = 127;
    const uint16_t major = 2, minor = 4;
    const int32_t zone = 0;
    const uint32_t sigfigs = 0;
    std::fwrite(&magic, 4, 1, _f);
    std::fwrite(&major, 2, 1, _f);
    std::fwrite(&minor, 2, 1, _f);
    std::fwrite(&zone, 4, 1, _f);
    std::fwrite(&sigfigs, 4, 1, _f);
    std::fwrite(&snap, 4, 1, _f);
    std::fwrite(&link, 4, 1, _f);
    return true;
  }
  void write(const uint8_t *radiotap, size_t rlen, const uint8_t *frame, size_t flen) {
    if (!_f) return;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = ((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10 - 11644473600000000ull;
    const uint32_t hdr[4] = {static_cast<uint32_t>(t / 1000000), static_cast<uint32_t>(t % 1000000),
                             static_cast<uint32_t>(rlen + flen), static_cast<uint32_t>(rlen + flen)};
    std::fwrite(hdr, sizeof(hdr), 1, _f);
    std::fwrite(radiotap, 1, rlen, _f);
    std::fwrite(frame, 1, flen, _f);
    if (++_n % 64 == 0) std::fflush(_f);
  }
  ~PcapWriter() {
    if (_f) std::fclose(_f);
  }

private:
  FILE *_f = nullptr;
  uint64_t _n = 0;
};

struct RadiotapInfo {
  size_t length = 0;
  bool has_rate = false, has_mcs = false;
  unsigned rate_x2 = 0, mcs = 0;
};

bool parse_radiotap(const uint8_t *p, size_t len, RadiotapInfo &info) {
  if (len < 8 || p[0] != 0) return false;
  info.length = p[2] | size_t(p[3]) << 8;
  if (info.length < 8 || info.length > len) return false;
  uint32_t present;
  std::memcpy(&present, p + 4, 4);
  size_t off = 8;
  uint32_t word = present;
  while (word & 0x80000000u) { /* extended bitmaps: skip them */
    if (off + 4 > info.length) return false;
    std::memcpy(&word, p + off, 4);
    off += 4;
  }
  static const struct { uint8_t align, size; } fields[] = {
      {8, 8}, {1, 1}, {1, 1}, {2, 4}, {2, 2}, {1, 1}, {1, 1}, {2, 2}, {2, 2}, {2, 2},
      {1, 1}, {1, 1}, {1, 1}, {1, 1}, {2, 2}, {2, 2}, {1, 1}, {1, 1}, {4, 8}, {1, 3}};
  for (unsigned bit = 0; bit < 20; bit++) {
    if (!(present & (1u << bit))) continue;
    off = (off + fields[bit].align - 1) & ~size_t(fields[bit].align - 1);
    if (off + fields[bit].size > info.length) return true;
    if (bit == 2) {
      info.has_rate = true;
      info.rate_x2 = p[off];
    } else if (bit == 19) {
      info.has_mcs = (p[off] & 0x02) != 0; /* MCS index known */
      info.mcs = p[off + 2];
    }
    off += fields[bit].size;
  }
  return true;
}

constexpr uint16_t kStaEthernet = 0x100; /* inbox: a frame from the TAP adapter */
constexpr uint16_t kScanRequest = 0x101; /* inbox: "scan" on stdin (the service's Wi-Fi list) */
constexpr uint16_t kRejoinRequest = 0x102; /* inbox: "rejoin" on stdin (the service's gateway probe failed) */

/* Networks heard in beacons and probe responses, for the client's Wi-Fi list. */
struct BssSeen {
  std::string ssid;
  uint8_t channel = 0;
  int8_t dbm = -127;
  std::string security; /* open, wep, wpa2-psk, wpa3-sae, wpa2/3-psk, owe, 802.1x */
  uint64_t seen_ms = 0;
};

std::string bss_security(const uint8_t *ies, size_t len, uint16_t capability) {
  bool psk = false, sae = false, dot1x = false, rsn = false, owe = false;
  for (size_t i = 0; i + 2 <= len;) {
    const uint8_t id = ies[i], n = ies[i + 1];
    if (i + 2 + n > len) break;
    if (id == 48 && n >= 8) {
      rsn = true;
      const uint8_t *r = ies + i + 2;
      size_t o = 2 + 4;
      const unsigned pc = r[o] | r[o + 1] << 8;
      o += 2 + 4u * pc;
      if (o + 2 <= n) {
        const unsigned ac = r[o] | r[o + 1] << 8;
        o += 2;
        for (unsigned k = 0; k < ac && o + 4 <= n; k++, o += 4) {
          if (r[o] != 0x00 || r[o + 1] != 0x0f || r[o + 2] != 0xac) continue;
          if (r[o + 3] == 2 || r[o + 3] == 6) psk = true;
          else if (r[o + 3] == 8) sae = true;
          else if (r[o + 3] == 18) owe = true;
          else if (r[o + 3] == 1 || r[o + 3] == 5) dot1x = true;
        }
      }
    }
    i += 2 + n;
  }
  if (rsn) {
    if (psk && sae) return "wpa2/3-psk";
    if (sae) return "wpa3-sae";
    if (psk) return "wpa2-psk";
    if (dot1x) return "802.1x";
    if (owe) return "owe";
    return "rsn";
  }
  return (capability & 0x0010) ? "wep" : "open";
}

const uint8_t kScanChannels[] = {1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,
                                 36,  40,  44,  48,  52,  56,  60,  64,  100, 104, 108, 112, 116,
                                 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165};

struct Request {
  uint16_t type;
  std::vector<uint8_t> payload;
};

class Mailbox {
public:
  void push(Request r) {
    std::lock_guard<std::mutex> l(_m);
    _q.push_back(std::move(r));
  }
  bool pop(Request &r) {
    std::lock_guard<std::mutex> l(_m);
    if (_q.empty()) return false;
    r = std::move(_q.front());
    _q.pop_front();
    return true;
  }
  size_t size() {
    std::lock_guard<std::mutex> l(_m);
    return _q.size();
  }

private:
  std::mutex _m;
  std::deque<Request> _q;
};

} // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parse_options(argc, argv, opt)) {
    std::fprintf(stderr,
                 "usage: %s <channel> [--width 20|40|80] [--country XX]\n"
                 "          [--mac-type listener|ibss|sta] [--firmware DIR]\n"
                 "          [--data-rate 6|12|24|36|48|54|MCS0..MCS15] [--verbose]\n",
                 argv[0]);
    return 2;
  }
  SetConsoleCtrlHandler(on_console, TRUE);
  timeBeginPeriod(1);

  PipeServer pipe(RADIO_LINK_PIPE_NAME, stopping);
  if (!pipe.valid()) {
    log_line("cannot create " + std::string(RADIO_LINK_PIPE_NAME) + " (error " +
             std::to_string(GetLastError()) + "): is another radio running?");
    return 1;
  }

  Mailbox inbox;
  std::atomic<bool> ready{false}, failed{false};
  std::atomic<bool> client_connected{false};
  uint8_t bssid_filter[6] = {};
  std::atomic<bool> have_filter{false};
  std::mutex filter_mutex;

  /* The device thread owns the card: bring-up, receive polling, sending. */
  /* The service asks for Wi-Fi scans on stdin. */
  std::thread stdin_thread([&] {
    char line[128];
    while (!stopping() && std::fgets(line, sizeof(line), stdin)) {
      if (std::strncmp(line, "quit", 4) == 0) {
        /* Let the device thread run dev.stop(): IwxDevice::stop() resets the
         * firmware and DMA. Killing the process skips it and can leave the
         * card in a degraded AWDL state after Windows takes it back. */
        g_stop = true;
        break;
      }
      if (std::strncmp(line, "rejoin", 6) == 0) {
        inbox.push({kRejoinRequest, {}});
        continue;
      }
      if (std::strncmp(line, "scan", 4) != 0) continue;
      /* "scan" or "scan <SSID in hex>" (a directed scan, for hidden networks) */
      std::vector<uint8_t> ssid;
      for (const char *h = line + 4; *h == ' ' || std::isxdigit(static_cast<unsigned char>(*h));) {
        if (*h == ' ') { h++; continue; }
        if (!std::isxdigit(static_cast<unsigned char>(h[1]))) break;
        char b[3] = {h[0], h[1], 0};
        if (ssid.size() < 32) ssid.push_back(static_cast<uint8_t>(std::strtoul(b, nullptr, 16)));
        h += 2;
      }
      inbox.push({kScanRequest, ssid});
    }
  });
  stdin_thread.detach();

  std::thread device_thread([&] {
    if (opt.no_card) {
      ready = true;
      log_line("test mode: no card; counting what the client sends");
      uint64_t tx = 0, action = 0, data = 0, other = 0, last = GetTickCount64();
      Request req;
      while (!stopping()) {
        while (inbox.pop(req)) {
          if (req.type == RADIO_LINK_TX) {
            RadiotapInfo rt;
            if (!parse_radiotap(req.payload.data(), req.payload.size(), rt)) {
              log_line("TX with a malformed radiotap header");
              continue;
            }
            const uint8_t fc = req.payload.size() > rt.length ? req.payload[rt.length] : 0;
            tx++;
            if (fc == 0xd0) action++;
            else if ((fc & 0x0c) == 0x08) data++;
            else other++;
          } else if (req.payload.size() == 6) {
            char b[64];
            std::snprintf(b, sizeof(b), "%s %02x:%02x:%02x:%02x:%02x:%02x",
                          req.type == RADIO_LINK_SET_ADDRESS ? "address" : "bssid filter",
                          req.payload[0], req.payload[1], req.payload[2], req.payload[3],
                          req.payload[4], req.payload[5]);
            log_line(b);
          } else if (req.type == RADIO_LINK_SET_CHANNEL && req.payload.size() == 1) {
            log_line("channel " + std::to_string(req.payload[0]));
          }
        }
        if (GetTickCount64() - last >= 2000) {
          last = GetTickCount64();
          log_line("client sent " + std::to_string(tx) + " frames: " + std::to_string(action) +
                   " action, " + std::to_string(data) + " data, " + std::to_string(other) +
                   " other");
        }
        Sleep(5);
      }
      return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    /* The same AWDL engine on either backend: the user-mode hardware layer,
     * or the kernel driver's listener (--kernel). */
    const auto run = [&](auto &dev) {
    /* What this backend can do (radio_backend.h), instead of asking whether
     * it is IwxDevice: every call outside the core listener sits under
     * `if constexpr` on its capability, so a backend implements only what it
     * declares (no stubs). Nested lambdas read Dev::kCapabilities directly. */
    using Dev = std::decay_t<decltype(dev)>;
    constexpr RadioCapabilities caps = Dev::kCapabilities;
    static_assert(radio::check_backend_contract<Dev>(), "radio backend contract");
    std::string error;
    if (!dev.open(opt.dev, error)) {
      log_line("bring-up failed: " + error);
      failed = true;
      g_stop = true;
      return;
    }
    /* One informational line for reports; it matches no pattern report.ps1
     * or Supervisor.cs reacts to (iwx_selftest checks the wording). */
    log_line(radio::format_capabilities_summary(caps));
    /* The station drives the backend through StaDevice (sta_device.h); the
     * wrapper is only named, never built, for a backend without a station. */
    using StaDev = std::conditional_t<caps.supports_station, StaDeviceOf<Dev>, int>;
    std::optional<StaDev> sta_dev;
    std::unique_ptr<WifiStation> sta;
    StaConfig sta_cfg;
    if (!opt.sta_config.empty()) {
      std::string err;
      if constexpr (caps.supports_station) {
        if (sta_cfg.load(opt.sta_config, err)) {
          if constexpr (std::is_same_v<Dev, IwxDevice>) {
            if (!sta_cfg.block_ack.has_value()) sta_cfg.block_ack = dev.uses_mld_listener();
          }
          sta_dev.emplace(dev);
          sta = std::make_unique<WifiStation>(*sta_dev, log_line);
        }
        else log_line("station config: " + err);
      } else {
        log_line(radio::station_policy_message(caps));
      }
      /* The file holds the Wi-Fi password: gone once read. */
      DeleteFileA(opt.sta_config.c_str());
    }
    std::vector<uint8_t> message;
    PcapWriter capture;
    const bool capture_on = !opt.pcap.empty() && capture.open(opt.pcap);
    if (!opt.pcap.empty()) log_line(capture_on ? "capturing to " + opt.pcap : "cannot write " + opt.pcap);
    uint8_t dev_address[6] = {};
    bool beacon_heard = false;
    uint64_t tx_held_passive = 0;
    uint8_t peer_seq[16] = {};
    unsigned peer5g = 0;
    awdl::PeerCountry peer_country;
    std::string want_country, have_country = opt.dev.country;
    /* Experimental fast unlock: start from what the firmware holds, not from
     * --country. Where the firmware refused it (#11: "00"), have_country ==
     * --country hid both the firmware's later notice of that country ("event:
     * country") and a peer naming it (no set_country retry). */
    if constexpr (caps.supports_country_notice) {
      if (opt.fast_unlock) have_country = radio::held_country(dev.current_country());
    }
    uint64_t rx_all = 0, rx_awdl = 0, rx_to_us = 0;
    /* Data-type frames handed up by the backend (before the A-MPDU reorder is
     * visible here they are already reordered); the split tells a receive
     * stall inside the radio/reorder from one above. */
    uint64_t rx_data = 0;
    /* AWDL action frames by subtype: a peer turns valid in awdld only after a
     * MIF (master indication), so PSF-only traffic explains "not valid yet"
     * (#16, AX211). */
    uint64_t rx_psf = 0, rx_mif = 0;
    /* Frames that came with their FCS (stripped here), and MIFs awdld would
     * still reject: bytes after the last TLV it cannot trim, or no version
     * TLV (#24/#25: an AX211/AX411 peer stayed "not valid yet"). */
    uint64_t rx_fcs = 0, rx_mif_tail = 0, rx_mif_nover = 0;
    /* --coex-plan: the peer's schedule (fed from its sync frames whether or
     * not A-MPDU is on), our router entries, and what the plan did. */
    struct Plan {
      awdl_sched::PeerSchedule sched{};
      uint8_t peer[6] = {}, away[16] = {}, router_ch = 0;
      bool have_peer = false, active = false;
      awdl_plan::InfraMeter meter;
      uint64_t last_data_us = 0, tick_us = 0;
      uint64_t rewritten = 0, silenced = 0, unaligned = 0, stale = 0;
      uint32_t changes = 0;
    } plan;
    uint32_t rx_data_to_us = 0;
    /* AWDL unicast data frames both ways, and the station's pause during
     * AWDL transfers (sta_yield.h). */
    uint64_t awdl_unicast_data = 0, yield_tick_us = 0;
    sta_yield::Controller yield;
    bool yield_announced = false;
    bool yield_he_noted = false;
    int8_t awdl_dbm = -127;
    /* Discovery timing (#4: ~99 s to see the iPhone, #11: ~2 min): one line
     * when the first AWDL frame, the first MIF and the first frame addressed
     * to us arrive, counted from when listening started. Diagnostic only. */
    uint64_t listen_ms = GetTickCount64();
    const auto milestone = [&](const char *what, const std::string &detail) {
      log_line(radio::format_discovery_milestone(what, GetTickCount64() - listen_ms, detail));
    };
    std::deque<awdl::AddbaRequest> addba_pending;
    struct Delba {
      uint8_t peer[6];
      uint8_t tid;
    };
    std::deque<Delba> delba_pending;
    /* Experimental A-MPDU sending to one peer (ForbiddenItlwm's
     * -itlawdlcoexbatx, awdlCoexBaHandle and awdlCoexDataPump): a bulk send to
     * a peer gets its firmware station, an aggregation queue and our ADDBA
     * request; once it agrees, its data goes out as QoS frames on TID 0 and
     * the firmware aggregates them. While the peer's own schedule has it off
     * our channel, its queue is paused and other frames for it wait. */
    struct Agg {
      enum Step { Off, Asking, Agreed } step = Off;
      bool on = false;
      uint8_t dest[6] = {}, peer[6] = {}, sched_peer[6] = {};
      bool have_dest = false, have_sched_peer = false;
      uint32_t dest_frames = 0, epoch = 0;
      awdl::BaTxBulk bulk;
      uint8_t token = 0x5a;
      unsigned asks = 0, resume_failures = 0, stalls = 0;
      uint16_t peer_window = awdl::kBaWindowMax;
      uint64_t ask_started_us = 0, last_ask_us = 0, rest_until_us = 0, last_peer_us = 0, last_agg_us = 0, retry_us = 0,
               agreed_us = 0;
      bool answered = false, refused = false, delba = false;
      std::deque<std::vector<uint8_t>> wait; /* frames for the peer, in order, for the aggregate queue */
      awdl_sched::PeerSchedule sched{};
      awdl_sched::PeerPauseState pause{};
      awdl_sched::PeerBulkState peer_bulk{};
      uint64_t sched_rx_us = 0, gate_since_us = 0, gate_limit_us = 0;
      uint8_t gate_kind = 0;
      uint64_t asks_total = 0, agreements = 0, refusals = 0, delbas = 0, setup_failures = 0, pause_errors = 0,
               gate_holds = 0, sync_frames = 0, dropped = 0;
      uint64_t agree_frames = 0, agree_notifs = 0, agree_singles = 0;
    } agg;
    agg.sched.reset();
    agg.sched.assumeZero = true; /* upstream's default (iface161) */
    plan.sched.assumeZero = true;
    /* The A-MPDU queue as seen from code that runs on every backend: one
     * without supports_aggregate never turns agg.on and has no queue. */
    const auto agg_ready = [&]() -> bool {
      if constexpr (Dev::kCapabilities.supports_aggregate) return dev.agg_ready();
      else return false;
    };
    const auto agg_queued = [&]() -> unsigned {
      if constexpr (Dev::kCapabilities.supports_aggregate) return dev.agg_queued();
      else return 0u;
    };
    /* The card's channel flags; a backend that does not enforce passive
     * channels reads every channel as active (radio::channel_allows_tx). */
    const auto chan_flags = [&](uint8_t ch) -> uint32_t {
      if constexpr (Dev::kCapabilities.enforces_passive_channel) {
        return dev.channel_flags(ch);
      } else {
        (void)ch;
        return radio::kChannelActiveFlag;
      }
    };
    const auto now_us = [] {
      static LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
      }();
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      /* Quotient and remainder: counter * 1000000 overflows after ~21 days of
       * uptime at 10 MHz (Codex review #13). */
      const uint64_t c = static_cast<uint64_t>(t.QuadPart), hz = static_cast<uint64_t>(f.QuadPart);
      return c / hz * 1000000ull + c % hz * 1000000ull / hz;
    };
    std::map<std::string, BssSeen> bss_seen;
    /* Bz schedules nothing per station: an AWDL peer listens for us right
     * after it transmits, and frames sent at other times go unacknowledged.
     * Remember when each peer (frame address 2) was last heard, and hold
     * unicast and multicast until then (2026-11-25). */
    bool bz_card = false;
    if constexpr (std::is_same_v<Dev, IwxDevice>) bz_card = dev.bz_card();
    /* PR #95's AX203 fixes (declined receive Block Ack, station pause during
     * transfers) are measured on AX203 only; BE200 keeps its old behavior. */
    bool ax203 = false;
    if constexpr (std::is_same_v<Dev, IwxDevice>) ax203 = dev.ax203_card();
    struct PeerHeard {
      uint8_t addr[6] = {};
      uint64_t us = 0;
    };
    PeerHeard peers[16];
    unsigned peers_at = 0;
    uint64_t peer_any_us = 0;
    constexpr uint64_t kPeerWindowUS = 100000, kPeerHoldUS = 2000000;
    const auto note_peer = [&](const uint8_t *a, uint64_t t) {
      for (auto &p : peers)
        if (p.us && std::memcmp(p.addr, a, 6) == 0) {
          p.us = t;
          peer_any_us = t;
          return;
        }
      PeerHeard &p = peers[peers_at++ % (sizeof(peers) / sizeof(peers[0]))];
      std::memcpy(p.addr, a, 6);
      p.us = t;
      peer_any_us = t;
    };
    const auto peer_age = [&](const uint8_t *a, uint64_t t) -> uint64_t {
      for (const auto &p : peers)
        if (p.us && std::memcmp(p.addr, a, 6) == 0) return t - p.us;
      return UINT64_MAX;
    };
    bool scan_wanted = false, scan_started = false;
    std::vector<uint8_t> scan_ssid;
    uint64_t scan_started_ms = 0;
    /* What each peer advertises in its 16-entry channel sequence (TLV 18),
     * logged when it changes. A macOS listener pinned to another channel
     * advertises its station channel rather than the one it listens on, so a
     * "device X is not visible" report needs this before anything else. */
    struct SeqSeen {
      uint8_t mac[6] = {};
      uint8_t seq[16] = {};
      bool used = false;
    };
    SeqSeen seq_seen[4];
    const auto note_sequence = [&](const uint8_t *from, const uint8_t *frame, size_t len) {
      uint8_t seq[16];
      if (!awdl::peer_sequence(frame, len, seq)) return;
      SeqSeen *slot = nullptr;
      for (auto &s : seq_seen) {
        if (s.used && std::memcmp(s.mac, from, 6) == 0) { slot = &s; break; }
        if (!s.used && !slot) slot = &s;
      }
      if (!slot) return;
      if (slot->used && std::memcmp(slot->seq, seq, 16) == 0) return;
      const bool had = slot->used;
      slot->used = true;
      std::memcpy(slot->mac, from, 6);
      std::memcpy(slot->seq, seq, 16);
      std::string list;
      for (unsigned k = 0; k < 16; k++) list += (k ? "," : "") + std::to_string(seq[k]);
      char b[24];
      std::snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", from[0], from[1], from[2], from[3],
                    from[4], from[5]);
      log_line(std::string("event: peer-sequence ") + b + " " + list + (had ? " (changed)" : ""));
    };
    dev.set_rx_handler([&](const RxFrame &in) {
      rx_all++;
      /* RxFrame promises no FCS; some firmware appends it anyway. */
      RxFrame f = in;
      if (const size_t n = awdl::strip_valid_fcs(f.data, f.len); n != f.len) {
        f.len = n;
        if (++rx_fcs == 1) log_line("rx: frames arrive with their FCS; removing it");
      }
      if (bz_card && f.len >= 16 && !(f.data[10] & 1) &&
          std::memcmp(f.data + 10, dev_address, 6) != 0)
        note_peer(f.data + 10, now_us());
      if (f.len >= 2 && (f.data[0] & 0x0c) == 0x08) rx_data++;
      if (f.len >= 36 && (f.data[0] == 0x80 || f.data[0] == 0x50)) { /* beacon, probe response */
        char key[18];
        std::snprintf(key, sizeof(key), "%02x:%02x:%02x:%02x:%02x:%02x", f.data[16], f.data[17], f.data[18],
                      f.data[19], f.data[20], f.data[21]);
        if (!bss_seen.count(key) && bss_seen.size() >= 256) {
          /* Full: the entry heard longest ago goes (Codex review #31). */
          auto oldest = bss_seen.begin();
          for (auto it = bss_seen.begin(); it != bss_seen.end(); ++it)
            if (it->second.seen_ms < oldest->second.seen_ms) oldest = it;
          bss_seen.erase(oldest);
        }
        {
          BssSeen &b = bss_seen[key];
          const uint8_t *ies = f.data + 36;
          const size_t ies_len = f.len - 36;
          uint8_t ch = f.channel;
          for (size_t i = 0; i + 2 <= ies_len;) {
            const uint8_t id = ies[i], n = ies[i + 1];
            if (i + 2 + n > ies_len) break;
            if (id == 0 && n <= 32) b.ssid.assign(reinterpret_cast<const char *>(ies + i + 2), n);
            else if ((id == 3 || id == 61) && n >= 1 && ies[i + 2]) ch = ies[i + 2];
            i += 2 + n;
          }
          b.channel = ch;
          b.dbm = f.dbm;
          b.security = bss_security(ies, ies_len, static_cast<uint16_t>(f.data[34] | f.data[35] << 8));
          b.seen_ms = GetTickCount64();
        }
      }
      /* Beacon hint: a beacon heard in our 80 MHz block lifts "passive". Checked
       * before the station takes its AP's beacons: with Wi-Fi joined on the
       * AWDL channel those are the only beacons there, and AWDL would stay
       * held forever (2026-11-17). */
      if (f.len >= 36 && (f.data[0] == 0x80 || f.data[0] == 0x50) && !beacon_heard) {
        const unsigned a = awdl::block80_first(f.channel), b = awdl::block80_first(dev.channel());
        if ((a && a == b) || f.channel == dev.channel()) {
          beacon_heard = true;
          log_line("beacon heard on channel " + std::to_string(f.channel) + ": transmitting allowed here");
        }
      }
      if (sta && sta->owns(f)) {
        sta->on_frame(f);
        return;
      }
      if (agg.on && f.len >= 16 && !(f.data[10] & 1) && std::memcmp(f.data + 10, dev_address, 6) != 0 &&
          f.channel == dev.channel()) {
        /* The peer's schedule, from its sync frames: the Block Ack peer once
         * there is one, else the first peer heard (upstream's selection). */
        const uint8_t *from = f.data + 10;
        const uint64_t heard = now_us();
        const bool is_sched = agg.have_sched_peer && std::memcmp(from, agg.sched_peer, 6) == 0;
        const bool want = agg.step != Agg::Off ? std::memcmp(from, agg.peer, 6) == 0
                                               : is_sched || !agg.sched.fresh(heard);
        if (want && awdl::is_awdl_action(f.data, f.len)) {
          awdl_sched::PeerSyncInfo info{};
          bool bad = false;
          if (awdl_sched::peerSyncFromAction(f.data, f.len, &info, &bad)) {
            if (!is_sched) {
              agg.sched.reset();
              std::memcpy(agg.sched_peer, from, 6);
              agg.have_sched_peer = true;
            }
            agg.sched.notePeerFrame(heard);
            agg.sched.note(info, heard, dev.channel());
            agg.sync_frames++;
          } else if (is_sched) {
            if (bad) agg.sched.invalidate();
            else agg.sched.notePeerFrame(heard);
          }
        } else if (is_sched) {
          agg.sched_rx_us = heard;
          agg.sched.notePeerFrame(heard);
        }
      }
      if (opt.coex_plan && f.len >= 16 && !(f.data[10] & 1) && std::memcmp(f.data + 10, dev_address, 6) != 0 &&
          f.channel == dev.channel() && awdl::is_awdl_action(f.data, f.len)) {
        const uint8_t *from = f.data + 10;
        const uint64_t heard = now_us();
        const bool is_peer = plan.have_peer && std::memcmp(from, plan.peer, 6) == 0;
        if (is_peer || !plan.sched.fresh(heard)) {
          awdl_sched::PeerSyncInfo info{};
          if (awdl_sched::peerSyncFromAction(f.data, f.len, &info)) {
            if (!is_peer) {
              plan.sched.reset();
              std::memcpy(plan.peer, from, 6);
              plan.have_peer = true;
            }
            plan.sched.notePeerFrame(heard);
            plan.sched.note(info, heard, dev.channel());
          } else if (is_peer) {
            plan.sched.notePeerFrame(heard);
          }
        }
      }
      if (awdl::is_awdl_action(f.data, f.len)) {
        if (++rx_awdl == 1)
          milestone("AWDL action frame",
                    " (channel " + std::to_string(f.channel) + ", " + std::to_string(f.dbm) + " dBm)");
        /* Peer hint: an Apple device sending AWDL on our channel has the channel
         * open in its own country. Needed where the firmware refuses the
         * country (MCC status 4, "00" world domain: all of 5 GHz passive) and
         * no access point beacons near the AWDL channel (#11, AX210, TW). */
        if (!beacon_heard && f.channel == dev.channel() && dev.channel() > 14) {
          beacon_heard = true;
          log_line("AWDL peer heard on channel " + std::to_string(f.channel) + ": transmitting allowed here");
        }
        if (f.len > 30) {
          if (f.data[30] == 0) rx_psf++;      /* periodic synchronization */
          else if (f.data[30] == 3 && ++rx_mif == 1) milestone("AWDL MIF", ""); /* master indication */
          if (f.data[30] == 3) {
            const awdl::AwdlTlvFit fit = awdl::awdl_tlv_fit(f.data, f.len);
            if (!fit.has_version) rx_mif_nover++;
            if (fit.left && !fit.left_zero && ++rx_mif_tail <= 3) {
              std::string tail;
              const size_t from = f.len > 16 ? f.len - 16 : 0;
              for (size_t i = from; i < f.len; i++) {
                char h[4];
                std::snprintf(h, sizeof(h), "%02x", f.data[i]);
                tail += h;
              }
              log_line("rx: MIF of " + std::to_string(f.len) + " bytes has " + std::to_string(fit.left) +
                       " bytes after its last TLV (awdld drops it); last bytes " + tail);
            }
          }
        }
        awdl_dbm = f.dbm;
        /* The sequence itself, not just the peer channel derived from it. */
        note_sequence(f.data + 10, f.data, f.len);
        uint8_t seq[16];
        if (awdl::peer_sequence(f.data, f.len, seq) && std::memcmp(seq, peer_seq, 16) != 0) {
          std::memcpy(peer_seq, seq, 16);
          const unsigned ch = awdl::peer_channel(seq);
          if (ch && ch != peer5g && allow_peer_channel(ch)) {
            peer5g = ch;
            log_line("event: peer-channel " + std::to_string(ch));
          }
        }
        char c[3];
        if (awdl::peer_country(f.data, f.len, c) && peer_country.note(c)) {
          log_line("event: peer-country " + std::string(peer_country.confirmed));
          want_country = peer_country.confirmed; /* applied from the main loop */
        }
      }
      if (f.len >= 10 && std::memcmp(f.data + 4, dev_address, 6) == 0) {
        if (++rx_to_us == 1) milestone("frame addressed to us", "");
        if ((f.data[0] & 0x0c) == 0x08) {
          rx_data_to_us++;
          awdl_unicast_data++;
          plan.last_data_us = now_us();
        }
        awdl::AddbaRequest req;
        bool sender = false;
        uint8_t tid = 0;
        awdl::AddbaAnswer ans;
        if (awdl::parse_addba_request(f.data, f.len, dev_address, &req)) {
          if (addba_pending.size() < 16) addba_pending.push_back(req);
        } else if (awdl::parse_delba(f.data, f.len, dev_address, &sender, &tid)) {
          if (sender) {
            Delba d{};
            std::memcpy(d.peer, f.data + 10, 6);
            d.tid = tid;
            if (delba_pending.size() < 16) delba_pending.push_back(d);
          }
          else if (agg.on && tid == 0 && std::memcmp(f.data + 10, agg.peer, 6) == 0) agg.delba = true;
        } else if (agg.on && agg.step == Agg::Asking && awdl::parse_addba_answer(f.data, f.len, dev_address, &ans) &&
                   std::memcmp(f.data + 10, agg.peer, 6) == 0 && ans.token == agg.token && ans.tid == 0) {
          if (ans.status == 0 && ans.immediate) {
            agg.peer_window = (ans.window && ans.window < awdl::kBaWindowMax) ? ans.window : awdl::kBaWindowMax;
            agg.answered = true;
          } else {
            agg.refused = true;
          }
        }
      }
      if (capture_on) {
        /* Radiotap: flags, channel (frequency + flags) and dBm signal, so a
         * capture says which channel each frame arrived on. flags at 8,
         * channel at 10 (2-byte aligned), signal at 14. */
        uint8_t rt[15] = {0, 0, 15, 0};
        const uint32_t present = (1u << 1) | (1u << 3) | (1u << 5);
        std::memcpy(rt + 4, &present, 4);
        const uint16_t freq = f.channel <= 14 ? uint16_t(2407 + 5 * f.channel)
                                              : uint16_t(5000 + 5 * f.channel);
        std::memcpy(rt + 10, &freq, 2);
        const uint16_t cflags = f.channel <= 14 ? 0x00a0 : 0x0140; /* CCK+OFDM / 5 GHz OFDM */
        std::memcpy(rt + 12, &cflags, 2);
        rt[14] = static_cast<uint8_t>(f.dbm);
        capture.write(rt, sizeof(rt), f.data, f.len);
      }
      if (!client_connected || f.len < 24) return;
      if (have_filter) {
        std::lock_guard<std::mutex> l(filter_mutex);
        if (std::memcmp(f.data + 16, bssid_filter, 6) != 0) return;
      }
      /* The radiotap header winawdl's radio sends: flags and signal. */
      constexpr size_t kRadiotapBytes = 10;
      message.assign(kRadiotapBytes, 0);
      message[2] = kRadiotapBytes;
      const uint32_t present = (1u << 1) | (1u << 5);
      std::memcpy(&message[4], &present, 4);
      message[8] = 0;
      message[9] = static_cast<uint8_t>(f.dbm);
      message.insert(message.end(), f.data, f.data + f.len);
      if (message.size() <= RADIO_LINK_MAX_PAYLOAD) pipe.queue_send(RADIO_LINK_RX, message);
    });
    if (!dev.start_monitor(opt.channel, opt.width, dev.hw_address().data())) {
      log_line("could not start listening");
      failed = true;
      g_stop = true;
      dev.stop();
      return;
    }
    listen_ms = GetTickCount64();
    std::thread tap_thread;
    if (sta) {
      std::string err;
      if (!sta->start(sta_cfg, err)) {
        log_line("station: " + err);
        log_line("event: wifi failed");
        sta.reset();
      } else {
        tap_thread = std::thread([&] {
          std::vector<uint8_t> eth;
          while (!stopping()) {
            if (!sta->tap().read(eth, 200)) {
              Sleep(50);
              continue;
            }
            if (!eth.empty() && inbox.size() < 4096) inbox.push({kStaEthernet, eth});
          }
        });
      }
    }
    log_line(std::string("event: channel ") + std::to_string(opt.channel) + " " +
             (radio::channel_active(opt.channel, chan_flags(opt.channel)) ? "active" : "passive"));

    /* Host-side resend (ForbiddenItlwm's ResendState): frames to one station
     * go out one at a time; a failed one is sent again, with the retry flag,
     * for up to 400 ms, since the peer may be on another channel. */
    struct Unicast {
      std::vector<uint8_t> frame;
      TxRate rate;
      bool data;
      awdl::UnicastRetry tx{};
      uint64_t queued_us = 0; /* Bz: when it entered the queue (peer gating) */
    };
    std::deque<Unicast> uq;
    /* Broadcast/multicast from the client, waiting for a peer to listen (Bz). */
    std::deque<Unicast> mcastq;
    /* The firmware data ring holds 128 (AX210) / 256 entries. Sending one
     * frame and waiting for its TX-status reply serialises a round trip into
     * every packet (~1 ms), which caps an AWDL upload near 1 MB/s: a large
     * file then overflows uq (1024), drops frames and the peer's TCP/TLS
     * connection dies. Keep a small window of frames in flight instead; the
     * station path already sends that way. Management frames keep their
     * per-frame retry so ADDBA answers stay reliable. */
    /* Window, batched doorbell and ladder start, per backend
     * (radio::resolve_tx_config): above 8 the doorbell is rung once per batch
     * where the backend has one, and faster sending (experimental) starts one
     * step up, at HT MCS 4: a short send spent most of itself climbing from
     * legacy 24 (#4). */
    const radio::TxConfig txc = radio::resolve_tx_config(opt.tx_window, caps);
    if (txc.window_clamped)
      log_line("tx window " + std::to_string(opt.tx_window) + " is above what backend " + caps.backend +
               " takes: using " + std::to_string(txc.window));
    const unsigned kTxWindow = txc.window;
    const bool batch_kick = txc.batch_kick;
    constexpr size_t kQueueLimit = 4096; /* large uploads must not overflow at 1024 */
    unsigned inflight = 0;
    uint64_t resent = 0, gave_up = 0, uq_dropped = 0, missing_status = 0;
    awdl::RateLadder ladder;
    ladder.step = txc.initial_rate_step;
    bool ladder_seeded = false;
    const bool wide80 = opt.channel > 14 && opt.width >= 80;
    const auto rate_for_step = [&](unsigned st) -> TxRate {
      TxRate r;
      if (st == 0) {
        r.kind = TxRate::Legacy;
        r.mbps_x2 = 48;
      } else if (st <= 3) {
        r.kind = TxRate::Ht;
        r.mcs = awdl::kRateMcs[st];
      } else {
        r.kind = TxRate::Vht;
        r.mcs = awdl::kRateMcs[st];
        r.nss = 2;
        r.wide80 = true;
      }
      return r;
    };
    const auto data_rate = [&]() -> TxRate {
      if (opt.fixed_rate) return opt.data_rate;
      return rate_for_step(ladder.step);
    };
    dev.set_tx_status_handler([&](unsigned slot, bool acked) {
      /* Find the frame this slot belongs to. With a window several frames are
       * outstanding; only their own status may retire or retry them. */
      for (auto &f : uq) {
        auto &tx = f.tx;
        if (tx.slot < 0 || slot != static_cast<unsigned>(tx.slot)) continue;
        if (inflight) --inflight;
        tx.slot = -1;
        const uint64_t now = now_us();
        if (f.data && tx.attempts == 1) ladder.note(acked, wide80);
        if (acked) {
          tx.acked = true; /* the pump pops completed frames from the front */
          return;
        }
        if (tx.expired(now)) {
          if (f.data && !opt.fixed_rate) ladder.note_give_up();
          tx.abandon = true;
          gave_up++;
          return;
        }
        f.frame[1] |= 0x08; /* retry */
        tx.next_us = now + awdl::resend_backoff_us(tx.attempts);
        resent++;
        return;
      }
    });
    const auto pump = [&] {
      const uint64_t now = now_us();
      /* Retire completed/abandoned frames strictly from the front so the peer
       * sees sequence numbers in order. */
      bool removed = false;
      while (!uq.empty() && uq.front().tx.finished()) {
        uq.pop_front();
        removed = true;
      }
      /* A missing completion outlives the resend window. Retire the frame
       * and release the host window; recycling it can block new TCP ACKs. */
      for (auto &f : uq) {
        auto &tx = f.tx;
        if (tx.status_missing(now)) {
          if (inflight) --inflight;
          tx.slot = -1;
          tx.abandon = true;
          missing_status++;
          gave_up++;
          removed = true;
        }
      }
      /* Upstream's send gate (A-MPDU sending only, once an aggregation session
       * is agreed): data frames for the schedule peer wait while its schedule
       * has it away or dozing, for at most the away run. Never gate while idle
       * or while asking for Block Ack (otherwise ADDBA requests and single-frame
       * traffic stall behind gate holds, #4), and never gate management frames. */
      bool gate_closed = false;
      if (agg.on && agg.step == Agg::Agreed && agg.have_sched_peer && agg.sched.known() && agg.sched.fresh(now)) {
        const bool dozing = agg.sched.presentEntries() <= awdl_sched::kPeerDozeMaxEntries &&
                            (!agg.sched_rx_us || now - agg.sched_rx_us > 100000);
        const auto gate = agg.sched.sendGate(now, dozing, 4000);
        if (gate == awdl_sched::PeerSchedule::SendGate::Open) {
          agg.gate_since_us = 0;
          agg.gate_kind = 0;
        } else {
          const uint8_t kind = static_cast<uint8_t>(gate);
          if (!agg.gate_since_us || kind != agg.gate_kind) {
            if (!agg.gate_since_us) agg.gate_holds++;
            agg.gate_since_us = now;
            agg.gate_kind = kind;
            agg.gate_limit_us = gate == awdl_sched::PeerSchedule::SendGate::Away ? agg.sched.pauseLimitUS(now)
                                                                                  : agg.sched.awUS() + 2000;
          }
          if (now - agg.gate_since_us < agg.gate_limit_us) gate_closed = true;
          else agg.gate_limit_us = 0;
        }
      } else {
        agg.gate_since_us = 0;
        agg.gate_kind = 0;
      }
      /* Fill the window, skipping frames that are still waiting on a status
       * or on their retry backoff. */
      bool sent = false;
      for (auto &f : uq) {
        auto &tx = f.tx;
        if (!tx.ready(now)) continue;
        if (bz_card && f.data && f.frame.size() >= 10 && !(f.frame[4] & 1)) {
          /* Wait for the peer's listening window, but never forever. */
          if (!f.queued_us) f.queued_us = now;
          const uint64_t age = peer_age(f.frame.data() + 4, now);
          if (age != UINT64_MAX && age > kPeerWindowUS && now - f.queued_us < kPeerHoldUS)
            continue;
        }
        if (tx.expired(now)) {
          if (f.data && !opt.fixed_rate) ladder.note_give_up();
          tx.abandon = true;
          gave_up++;
          removed = true;
          continue;
        }
        if (inflight >= kTxWindow) break;
        if (gate_closed && f.data && f.frame.size() >= 10 &&
            std::memcmp(f.frame.data() + 4, agg.sched_peer, 6) == 0)
          continue;
        if (!tx.attempts) {
          tx.first_us = now;
          if (f.data) {
            if (!ladder_seeded && !opt.fixed_rate && awdl_dbm > -120 && ladder.ups == 0 && ladder.downs == 0 &&
                ladder.ok == 0 && ladder.failed == 0) {
              ladder.step = awdl::rssi_initial_rate_step(awdl_dbm, wide80, txc.initial_rate_step);
              ladder_seeded = true;
            }
            f.rate = data_rate();
          }
        } else if (f.data && !opt.fixed_rate) {
          f.rate = rate_for_step(awdl::retry_rate_step(ladder.step, tx.attempts));
        }
        const int slot = dev.transmit_slot(f.frame.data(), f.frame.size(), f.rate, !batch_kick);
        if (slot < 0) break; /* ring full: try again next tick */
        tx.slot = slot;
        tx.attempts++;
        tx.sent_us = now;
        inflight++;
        sent = true;
      }
      if constexpr (Dev::kCapabilities.supports_batched_kick) {
        if (sent && batch_kick) dev.kick_data(); /* one doorbell for the batch */
      }
      return sent || removed;
    };
    const auto may_send = [&] {
      return radio::channel_allows_tx(dev.channel(), chan_flags(dev.channel()), beacon_heard, Dev::kCapabilities);
    };
    bool channel_open = radio::channel_active(dev.channel(), chan_flags(dev.channel()));
    /* One scan, 10 s after start, while the AWDL channel is passive and no
     * beacon unlocked it: access points' Country elements let the firmware
     * learn the country and send its notice (ForbiddenItlwm's discovery scan).
     * With --fast-unlock (experimental) up to three, sooner and shorter
     * (radio::passive_discovery_scan_delay_ms, fast_discovery_channels). */
    const uint64_t discovery_from = GetTickCount64();
    unsigned discovery_scans = 0;       /* discovery scans due so far */
    bool fast_discovery_wanted = false; /* the next scan is a fast discovery scan */
    bool fast_discovery_running = false;
    uint8_t fast_channels[radio::kFastDiscoveryChannelCount] = {};
    const size_t fast_channel_count = radio::fast_discovery_channels(opt.channel, fast_channels);
    if (opt.fast_unlock) log_line(radio::kFastUnlockOnLine);
    if (opt.coex_plan) log_line("experimental coexistence plan: on (router entries in our schedule when the station is on another 5 GHz channel)");
    if (opt.sta_yield_hold) {
      yield.idle_us = 30000000;
      yield.max_pause_us = 120000000;
    }
    if (sta && ax203)
      log_line(!opt.sta_yield        ? "station pause for AWDL transfers: off"
               : opt.sta_yield_hold ? "station pause for AWDL transfers: on, held until AWDL is idle for 30 s"
                                    : "station pause for AWDL transfers: on");
    if (opt.no_block_ack || (ax203 && !opt.bz_block_ack))
      log_line("Block Ack: peers' receive sessions declined (status 37); they send unaggregated");
    if constexpr (caps.supports_aggregate) agg.on = opt.aggregate && dev.agg_available();
    if (opt.aggregate) log_line(agg.on ? "A-MPDU sending: on" : "A-MPDU sending: not available on this path");
    awdl::BaFilter ba_filter;
    uint16_t mgmt_seq = 0;
    uint64_t ba_answers = 0, ba_declined = 0;
    ready = true;
    log_line("ready on channel " + std::to_string(opt.channel) + ", waiting for a client");

    const auto log_agg_snapshot = [&] {
      if constexpr (Dev::kCapabilities.supports_aggregate) {
        if (!agg.on) return;
        const auto &a = dev.agg_stats();
        static const char *const kStep[] = {"idle", "asking", "agreed"};
        char rate[16], sdetail[112] = {}, rdetail[32] = {};
        std::snprintf(rate, sizeof(rate), "0x%08x", a.last_rate);
        if (a.agg_resps) {
          std::snprintf(rdetail, sizeof(rdetail), ", agg-tx %llu", static_cast<unsigned long long>(a.agg_resps));
        }
        if (a.singles) {
          std::snprintf(sdetail, sizeof(sdetail), " (ok %llu, fail %llu, last 0x%04x, rate 0x%08x)",
                        static_cast<unsigned long long>(a.singles_ok),
                        static_cast<unsigned long long>(a.singles_fail),
                        static_cast<unsigned>(a.last_fail_status),
                        static_cast<unsigned>(a.last_single_rate));
        }
        log_line(std::string("aggregate: ") + kStep[agg.step] + (agg.pause.paused ? ", paused" : "") + "; queue " +
                 std::to_string(dev.agg_queued()) + ", waiting " + std::to_string(agg.wait.size()) + "; " +
                 std::to_string(a.frames) + " frames, " + std::to_string(a.notifs) + " Block Acks (" +
                 std::to_string(a.txed) + " sent, " + std::to_string(a.acked) + " acked, largest " +
                 std::to_string(a.peak_frames) + ", rate " + rate + rdetail + "), " + std::to_string(a.singles) +
                 " singles" + sdetail + "; asks " + std::to_string(agg.asks_total) + ", agreed " +
                 std::to_string(agg.agreements) + ", refused " + std::to_string(agg.refusals) + ", DELBA " +
                 std::to_string(agg.delbas) + "; pauses " + std::to_string(agg.pause.pauses) + " (forced " +
                 std::to_string(agg.pause.forcedResumes) + ", errors " + std::to_string(agg.pause_errors) +
                 "), gate holds " + std::to_string(agg.gate_holds) + "; schedule " +
                 (!agg.sched.valid ? "none"
                  : agg.sched.calibrated ? "calibrated"
                  : agg.sched.known() ? "assumed"
                                      : "uncalibrated") +
                 ", " + std::to_string(agg.sched.valid ? agg.sched.presentEntries() : 0u) + "/16 entries here, " +
                 std::to_string(agg.sync_frames) + " sync frames");
      }
    };
    uint64_t idle_rounds = 0, last_report = GetTickCount64(), last_health = 0, last_rejoin = 0;
    std::deque<std::pair<uint64_t, uint64_t>> stall_hist; /* station packets in, out at each report */
    WifiStation::State published_wifi = WifiStation::State::Idle;
    Request req;
    while (!stopping() && dev.healthy()) {
      bool busy = dev.poll() > 0;
      if (bz_card && !mcastq.empty()) {
        /* In the peer's listening window, or after holding it long enough. */
        const uint64_t t = now_us();
        if ((peer_any_us && t - peer_any_us <= kPeerWindowUS) ||
            t - mcastq.front().queued_us >= kPeerHoldUS) {
          dev.transmit(mcastq.front().frame.data(), mcastq.front().frame.size(),
                       mcastq.front().rate);
          mcastq.pop_front();
          busy = true;
        }
      }
      pipe.flush_send();
      if (sta) sta->begin_tx_batch();
      for (int n = 0; n < 128 && (inbox.pop(req) || (pipe.pop_shm_tx(req.payload) && (req.type = RADIO_LINK_TX, true))); n++) {
        busy = true;
        if (req.type == RADIO_LINK_TX) {
          RadiotapInfo rt;
          if (!parse_radiotap(req.payload.data(), req.payload.size(), rt)) continue;
          const uint8_t *frame = req.payload.data() + rt.length;
          const size_t len = req.payload.size() - rt.length;
          if (len < 10) continue;
          TxRate rate;
          const bool unicast = !(frame[4] & 1);
          const bool data = (frame[0] & 0x0c) == 0x08;
          if (data && unicast) awdl_unicast_data++;
          if (opt.coex_plan) {
            const uint64_t t = now_us();
            if (data && unicast) {
              plan.last_data_us = t;
            } else if (plan.active && awdl::is_awdl_action(frame, len)) {
              /* Our sync frame from awdld: silent in a router entry, else
               * the router named in those entries. Only while awdld's AW
               * counter is in step with the peer's. */
              uint8_t *w = req.payload.data() + rt.length;
              if (!plan.sched.fresh(t) || !plan.sched.eawUS) {
                plan.stale++;
              } else {
                const uint32_t eaw = plan.sched.eawAt(t, nullptr);
                if (!awdl_plan::in_step(awdl_plan::frame_aw_counter(w, len), eaw)) {
                  plan.unaligned++;
                } else if (plan.away[eaw % 16]) {
                  plan.silenced++;
                  continue;
                } else if (awdl_plan::rewrite_frame(w, len, plan.away, plan.router_ch)) {
                  plan.rewritten++;
                }
              }
            }
          }
          if (rt.has_mcs) {
            rate.kind = TxRate::Ht;
            rate.mcs = rt.mcs;
          } else if (rt.has_rate && rt.rate_x2) {
            rate.mbps_x2 = rt.rate_x2;
          }
          if (!may_send()) {
            tx_held_passive++; /* no initiating on a passive channel */
          } else if (unicast) {
            bool via_agg = false;
            if (agg.on && frame[0] == 0x08 && len >= 24) {
              const uint64_t t = now_us();
              if (!agg.have_dest || std::memcmp(frame + 4, agg.dest, 6) != 0) {
                std::memcpy(agg.dest, frame + 4, 6);
                agg.have_dest = true;
                agg.bulk = awdl::BaTxBulk{};
              }
              /* Payload-sized frames only, so incoming TCP ACK bursts do not start a bulk send. */
              if (len >= 1200) agg.dest_frames++;
              const bool to_peer = agg.step != Agg::Off && std::memcmp(frame + 4, agg.peer, 6) == 0;
              if (to_peer) agg.last_peer_us = t;
              /* One path, in order, for the peer while its aggregate queue
               * holds or waits for frames (upstream's awdlCoexOrder). */
              via_agg = to_peer && agg.step == Agg::Agreed && agg_ready() &&
                        (agg.bulk.bulk || len >= 1200 || !agg.wait.empty() || agg_queued() > 0 ||
                         (agg.last_agg_us && t - agg.last_agg_us < 300000));
            }
            if (via_agg) {
              if (agg.wait.size() >= kQueueLimit) agg.dropped++;
              else agg.wait.emplace_back(frame, frame + len);
            } else if (uq.size() >= kQueueLimit) uq_dropped++;
            else uq.push_back({std::vector<uint8_t>(frame, frame + len), rate, data, {}, now_us()});
          } else if (bz_card) {
            /* Bz: a broadcast is heard by whoever is listening, right after a
             * peer's own frame (sent from the main loop's tick). */
            if (mcastq.size() >= 256) uq_dropped++;
            else mcastq.push_back({std::vector<uint8_t>(frame, frame + len), rate, data, {}, now_us()});
          } else {
            dev.transmit(frame, len, rate);
          }
        } else if (req.type == kRejoinRequest) {
          if (sta && sta->yielded()) {
            log_line("wifi: rejoin ignored, the station is paused for an AWDL transfer");
          } else if (sta) {
            log_line("event: wifi-stall");
            sta->rejoin("the gateway stopped answering");
          }
        } else if (req.type == kScanRequest) {
          scan_wanted = true;
          scan_ssid = req.payload;
          fast_discovery_wanted = false; /* the service's full scan goes instead; it hears Country elements too */
        } else if (req.type == kStaEthernet) {
          if (sta) sta->on_ethernet(req.payload.data(), req.payload.size());
        } else if (req.type == RADIO_LINK_SET_ADDRESS && req.payload.size() == 6) {
          const bool updated = dev.set_address(req.payload.data());
          if constexpr (std::is_same_v<Dev, IwxDevice>) {
            if (dev.uses_mld_listener() && !updated) {
              log_line("could not set the firmware's AWDL address");
              failed = true;
              g_stop = true;
              break;
            }
          }
          std::memcpy(dev_address, req.payload.data(), 6);
        } else if (req.type == RADIO_LINK_SET_BSSID_FILTER && req.payload.size() == 6) {
          std::lock_guard<std::mutex> l(filter_mutex);
          std::memcpy(bssid_filter, req.payload.data(), 6);
          static const uint8_t zero[6] = {};
          have_filter = std::memcmp(bssid_filter, zero, 6) != 0;
        } else if (req.type == RADIO_LINK_SET_CHANNEL && req.payload.size() == 1) {
          const uint8_t ch = req.payload[0];
          dev.set_channel(ch, ch <= 14 ? 20 : opt.width);
        }
      }
      if (sta && sta->flush_tx()) busy = true;
      if (stopping()) break;
      /* Block Ack: open the firmware session, then answer from our queue. The
       * peer repeats its request until it hears an answer. */
      if constexpr (caps.supports_rx_ba) {
        while (!addba_pending.empty()) {
          const awdl::AddbaRequest r = addba_pending.front();
          addba_pending.pop_front();
          if (!r.immediate) continue;
          /* Bz: the firmware's receive sessions stall the peer. Each lasts
           * 1-3 s and carries 5-10 frames before the iPhone deletes it and
           * asks again, ~7 KB/s; the one fast upload went unaggregated
           * (hardware, AX203, 2026-11-26). */
          const bool decline = opt.no_block_ack || (ax203 && !opt.bz_block_ack);
          if (!decline && !dev.add_rx_ba(r.peer, r.tid, r.ssn, r.window, r.token)) continue;
          uint8_t answer[awdl::kAddbaResponseLength];
          awdl::build_addba_response(answer, r, dev_address, ++mgmt_seq, opt.he80,
                                     decline ? awdl::kAddbaDeclined : 0);
          if (may_send()) {
            uq.push_front({std::vector<uint8_t>(answer, answer + sizeof(answer)), TxRate{}, false});
            if (decline) ba_declined++;
            else ba_answers++;
          }
        }
        while (!delba_pending.empty()) {
          if constexpr (std::is_same_v<Dev, IwxDevice>)
            dev.remove_rx_ba(delba_pending.front().tid, delba_pending.front().peer);
          else
            dev.remove_rx_ba(delba_pending.front().tid);
          delba_pending.pop_front();
        }
      } else {
        /* No receive Block Ack: requests go unanswered, as when a session
         * cannot be opened, and the peer sends unaggregated. */
        addba_pending.clear();
        delba_pending.clear();
      }
      /* A-MPDU sending: compiled only for backends that declare it (agg.on
       * is never set on the others). */
      if constexpr (caps.supports_aggregate) if (agg.on) {
        const uint64_t t = now_us();
        const auto back_to_uq = [&] {
          /* Frames held for the aggregate queue go the ordinary way, first. */
          while (!agg.wait.empty()) {
            uq.push_front({std::move(agg.wait.back()), TxRate{}, true});
            agg.wait.pop_back();
          }
        };
        const auto stop = [&](const char *why) {
          /* Snapshot the live asking/agreed state before returning to idle (#4). */
          if (agg.step != Agg::Off) log_agg_snapshot();
          if (agg.step == Agg::Agreed) {
            dev.agg_enable(false);
            dev.agg_flush();
          }
          if (agg.pause.paused) dev.agg_pause(false);
          agg.pause = awdl_sched::PeerPauseState{};
          agg.gate_since_us = 0;
          agg.gate_kind = 0;
          back_to_uq();
          agg.step = Agg::Off;
          agg.answered = agg.refused = agg.delba = false;
          log_line(std::string("aggregate: stopped (") + why + ")");
        };
        /* The peer's station was replaced (a receive session with another peer). */
        if (agg.step != Agg::Off && dev.ba_epoch() != agg.epoch) stop("peer station replaced");
        const bool was_bulk = agg.bulk.bulk;
        agg.bulk.note(t, agg.dest_frames, agg.step == Agg::Agreed);
        /* Record a snapshot when a bulk send ends so reports never show only a
         * later idle line (#4). */
        if (was_bulk && !agg.bulk.bulk && agg.step != Agg::Off) log_agg_snapshot();
        if (agg.step == Agg::Off && agg.bulk.bulk && agg.have_dest && t >= agg.retry_us) {
          if (dev.agg_setup(agg.dest, opt.channel > 14 && opt.width >= 80, true)) {
            std::memcpy(agg.peer, agg.dest, 6);
            agg.epoch = dev.ba_epoch();
            agg.step = Agg::Asking;
            agg.asks = 0;
            agg.token++;
            agg.peer_window = awdl::kBaWindowMax;
            agg.ask_started_us = t;
            agg.last_ask_us = agg.rest_until_us = 0;
            agg.last_peer_us = t;
            agg.answered = agg.refused = agg.delba = false;
            /* Open promiscuous + mgmt receive BEFORE sending the first ADDBA
             * request so the peer's ADDBA response is never dropped (#4). */
            if (!dev.promiscuous()) dev.set_promiscuous(true);
            log_line("aggregate: bulk send to a peer; asking it for Block Ack");
          } else {
            agg.setup_failures++;
            agg.retry_us = t + 10000000;
            log_line("aggregate: could not set up the peer's station and queue; retrying in 10 s");
          }
        }
        if (agg.step == Agg::Asking) {
          if (agg.answered) {
            agg.answered = false;
            if (dev.agg_enable(true)) {
              const auto &a = dev.agg_stats();
              agg.step = Agg::Agreed;
              agg.agreed_us = t;
              agg.agreements++;
              agg.resume_failures = 0;
              agg.agree_frames = a.frames;
              agg.agree_notifs = a.notifs;
              agg.agree_singles = a.singles;
              log_line("aggregate: the peer agreed (window " + std::to_string(agg.peer_window) + "); sending A-MPDUs");
            } else {
              agg.setup_failures++;
              agg.retry_us = t + 10000000;
              stop("the firmware refused aggregation");
            }
          } else if (agg.refused) {
            agg.refused = false;
            agg.refusals++;
            agg.retry_us = t + awdl::kBaTxFailBackoffUS;
            stop("the peer refused Block Ack; falling back to single-frame queue");
          } else if (!agg.bulk.bulk && t - agg.last_peer_us > 15000000) {
            stop("peer idle during ADDBA negotiation");
          } else if (agg.asks >= awdl::kBaTxAskMax ||
                     (agg.ask_started_us && t - agg.ask_started_us >= awdl::kBaTxAskTimeoutUS)) {
            agg.retry_us = t + awdl::kBaTxFailBackoffUS;
            char detail[128];
            std::snprintf(detail, sizeof(detail),
                          "no ADDBA answer after %u asks; falling back to single-frame queue", agg.asks);
            stop(detail);
          } else if (t >= agg.rest_until_us &&
                     (!agg.last_ask_us || t - agg.last_ask_us >= awdl::kBaTxAskIntervalUS) &&
                     (agg.asks == 0 || !(agg.have_sched_peer && agg.sched.shouldPause(t)))) {
            if (may_send()) {
              static const uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
              uint8_t ask[awdl::kAddbaResponseLength];
              awdl::build_addba_request(ask, agg.peer, dev_address, kAwdlBssid, agg.token, 0, 64, dev.agg_seq(),
                                        ++mgmt_seq);
              uq.push_front({std::vector<uint8_t>(ask, ask + sizeof(ask)), TxRate{}, false});
              agg.asks++;
              agg.asks_total++;
              agg.last_ask_us = t;
            }
          }
        }
        if (agg.step == Agg::Agreed && agg.delba) {
          agg.delba = false;
          agg.delbas++;
          log_agg_snapshot();
          dev.agg_enable(false);
          if (agg.pause.paused) dev.agg_pause(false);
          dev.agg_flush();
          agg.pause = awdl_sched::PeerPauseState{};
          back_to_uq();
          agg.step = Agg::Asking;
          agg.asks = 0;
          agg.token++;
          agg.ask_started_us = t;
          agg.last_ask_us = 0;
          if (!dev.promiscuous()) dev.set_promiscuous(true);
          log_line("aggregate: the peer ended our Block Ack session; asking again");
        }
        if (agg.step == Agg::Agreed) {
          /* Pause the peer's queue while its schedule has it away (upstream's
           * awdlCoexBaPauseStep): only while aggregate frames move or wait. */
          const bool moving = agg.peer_bulk.note(t, static_cast<uint32_t>(dev.agg_stats().frames)) || agg.bulk.bulk ||
                              (agg.last_agg_us && t - agg.last_agg_us < 300000);
          const bool want = (moving || dev.agg_queued() > 0) && agg.have_sched_peer &&
                            std::memcmp(agg.sched_peer, agg.peer, 6) == 0 && agg.sched.shouldPause(t);
          const auto before = agg.pause;
          const int change = agg.pause.step(want, t, agg.sched.pauseLimitUS(t));
          if (change && !dev.agg_pause(change > 0)) {
            agg.pause = before;
            agg.pause_errors++;
            /* A station left with transmit disabled blocks everything to the
             * peer: after five failed resumes aggregation stops. */
            if (change < 0 && ++agg.resume_failures >= 5) stop("the firmware would not resume the peer's queue");
          } else if (change < 0) {
            agg.resume_failures = 0;
          }
          const unsigned hw_cap = dev.agg_queue_cap();
          const unsigned agg_cap =
              (agg.peer_window < awdl::kBaWindowMax && agg.peer_window < hw_cap) ? agg.peer_window : hw_cap;
          bool agg_sent = false;
          while (agg.step == Agg::Agreed && !agg.wait.empty() && !agg.pause.paused && dev.agg_queued() < agg_cap) {
            std::vector<uint8_t> qos;
            if (!awdl::build_qos_data(agg.wait.front().data(), agg.wait.front().size(), 0, dev.agg_seq(), qos)) {
              uq.push_back({std::move(agg.wait.front()), TxRate{}, true});
              agg.wait.pop_front();
              continue;
            }
            if (dev.transmit_agg(qos.data(), qos.size(), false) < 0) break;
            agg.wait.pop_front();
            agg.last_agg_us = t;
            agg_sent = true;
          }
          if (agg_sent) dev.kick_agg(); /* one doorbell for the whole A-MPDU batch */
          /* The bulk send is over: end the session ourselves (DELBA), so the
           * next send asks again instead of sending A-MPDUs the peer may
           * have stopped taking (#4). */
          if (agg.step == Agg::Agreed && !agg.pause.paused &&
              awdl::ba_tx_session_idle(agg.bulk.bulk, agg.wait.size(), dev.agg_queued(), agg.last_agg_us,
                                       agg.agreed_us, t)) {
            static const uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
            uint8_t delba[awdl::kDelbaLength];
            awdl::build_delba(delba, agg.peer, dev_address, kAwdlBssid, 0, true, ++mgmt_seq);
            stop("bulk send finished; ended our Block Ack session");
            /* This session did produce Block Acks: forgive earlier stalls, so a
             * once-broken peer does not keep aggregation off for the rest of the
             * radio's life. */
            agg.stalls = 0;
            uq.push_front({std::vector<uint8_t>(delba, delba + sizeof(delba)), TxRate{}, false});
          }
          const auto &a = dev.agg_stats();
          if (agg.step == Agg::Agreed &&
              awdl::ba_tx_stalled_as_singles(a.frames - agg.agree_frames, a.notifs - agg.agree_notifs,
                                             a.singles - agg.agree_singles)) {
            char detail[160];
            std::snprintf(detail, sizeof(detail),
                          "no Block Acks after %llu frames (%llu singles: %llu ok, %llu failed, last fail 0x%04x, rate 0x%08x); falling back to single-frame queue",
                          static_cast<unsigned long long>(a.frames - agg.agree_frames),
                          static_cast<unsigned long long>(a.singles - agg.agree_singles),
                          static_cast<unsigned long long>(a.singles_ok),
                          static_cast<unsigned long long>(a.singles_fail),
                          static_cast<unsigned>(a.last_fail_status),
                          static_cast<unsigned>(a.last_single_rate));
            /* If experimental HE80 TLC stalled with 0 Block Acks, immediately
             * fall back to proven VHT80 and allow a quick retry instead of
             * counting it toward permanent aggregation disable. */
            if (dev.agg_using_he80()) {
              dev.agg_fallback_vht();
              agg.retry_us = t + 2000000;
              stop("HE80 TLC gave no Block Acks; falling back to VHT80");
            } else {
              /* A second stall means this card's firmware is probably not
               * reporting Block Acks (seen on an AX210, #39). Back off for a
               * long while rather than until the radio restarts: a later peer
               * or a channel change can still make aggregation work. */
              agg.retry_us = awdl::ba_tx_retry_us(++agg.stalls, t);
              stop(detail);
            }
            /* The peer still holds the session it agreed to: end it (DELBA), as
             * after a finished bulk send, so its receive side does not keep
             * waiting for aggregate sequence numbers that will never come (#39). */
            static const uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
            uint8_t delba[awdl::kDelbaLength];
            awdl::build_delba(delba, agg.peer, dev_address, kAwdlBssid, 0, true, ++mgmt_seq);
            uq.push_front({std::vector<uint8_t>(delba, delba + sizeof(delba)), TxRate{}, false});
            if (agg.stalls >= awdl::kBaTxStallLimit)
              log_line("aggregate: no Block Acks again; next attempt in " +
                       std::to_string(awdl::kBaTxLongBackoffUS / 1000000) + " s");
          }
        }
      }
      if constexpr (caps.supports_station) if (opt.sta_yield && sta && ax203) {
        const uint64_t t = now_us();
        if (t - yield_tick_us >= 100000) {
          yield_tick_us = t;
          /* The station failed, rejoined or could not pause meanwhile. */
          if (yield.paused && !sta->yielded()) yield.resumed(t);
          const auto &sp = dev.sta_params();
          /* Not on a Wi-Fi 6 (HE) link: resuming one after a pause timed out
           * LINK_CONFIG and the firmware asserted (#96, AX203, v2.3.4). The
           * pause was only proven on Wi-Fi 5 links (#95). */
          if (sp.he && !yield_he_noted) {
            yield_he_noted = true;
            log_line("station pause for AWDL transfers: skipped on this Wi-Fi 6 link (#96)");
          }
          const bool eligible = sta->state() == WifiStation::State::Running && sp.channel &&
                                sp.channel != dev.channel() && !sp.he;
          sta_yield::Action a = yield.tick(t, awdl_unicast_data, eligible);
          if (a == sta_yield::Action::Pause && !sta->set_yield(true)) {
            yield.refused(t);
            a = sta_yield::Action::None;
          } else if (a == sta_yield::Action::Resume) {
            sta->set_yield(false);
          }
          if (a != sta_yield::Action::None)
            log_line(std::string("station: ") + (yield.paused ? "paused" : "back on channel " + std::to_string(sp.channel)) +
                     " (" + sta_yield::why_name(yield.why) + "); " + std::to_string(yield.pauses) + " pauses, " +
                     std::to_string(yield.paused_us / 1000000) + " s paused, " +
                     std::to_string(sta->yield_dropped()) + " frames from Windows dropped");
          const bool on = yield.paused && sta->yielded();
          if (on != yield_announced) {
            yield_announced = on;
            log_line(std::string("event: wifi-yield ") + (on ? "on" : "off"));
          }
        }
      }
      if constexpr (caps.supports_station) if (opt.coex_plan) {
        const uint64_t t = now_us();
        if (t - plan.tick_us >= 100000) {
          plan.tick_us = t;
          const bool up = sta && sta->state() == WifiStation::State::Running && !sta->yielded();
          if (up) plan.meter.note(t, sta->rx_packets() + sta->tx_packets(), sta->rx_bytes() + sta->tx_bytes());
          const auto &sp = dev.sta_params();
          const bool moving = plan.last_data_us && t - plan.last_data_us < awdl_plan::kDataHoldUS;
          uint8_t away[16];
          const bool on = awdl_plan::plan_entries(up, sp.channel, sp.width, dev.channel(), moving,
                                                  up ? plan.meter.cls : awdl_plan::InfraClass::Idle, away);
          const uint8_t router = on ? sp.channel : 0;
          if (on != plan.active || router != plan.router_ch || std::memcmp(away, plan.away, 16) != 0) {
            std::memcpy(plan.away, away, 16);
            plan.active = on;
            plan.router_ch = router;
            plan.changes++;
            std::memcpy(plan.sched.ownAway, away, 16);
            std::memcpy(agg.sched.ownAway, away, 16);
            std::string entries;
            for (unsigned k = 0; k < 16; k++)
              if (away[k]) entries += (entries.empty() ? "" : ",") + std::to_string(k);
            log_line(on ? "coex plan: router on channel " + std::to_string(router) + " in entries " + entries +
                              (moving ? " (AWDL data, router " : " (AWDL idle, router ") +
                              (plan.meter.cls == awdl_plan::InfraClass::Streaming ? "streaming)"
                               : plan.meter.cls == awdl_plan::InfraClass::Data    ? "data)"
                                                                                  : "idle)")
                        : std::string("coex plan: inactive (station on the AWDL channel, on 2.4 GHz, or not joined)"));
          }
        }
      }
      /* Promiscuous receive:
       * - while asking for TX Block Ack: keep promiscuous + mgmt RX ON so the
       *   peer's ADDBA response is never dropped (#4);
       * - while TX Block Ack is agreed: keep promiscuous OFF (BA listener);
       * - otherwise follow RX Block Ack busy hysteresis. */
      if constexpr (caps.supports_promiscuous_toggle) {
        bool any_ba = false;
        if constexpr (caps.supports_rx_ba) {
          for (unsigned t = 0; t < 8; t++) any_ba |= dev.rx_ba_open(t);
        }
        if (agg.on && agg.step == Agg::Asking) {
          if (!dev.promiscuous()) dev.set_promiscuous(true);
        } else if (agg.on && agg.step == Agg::Agreed) {
          if (dev.promiscuous()) dev.set_promiscuous(false);
        } else if (any_ba && ba_filter.note(now_us(), rx_data_to_us)) {
          dev.set_promiscuous(!ba_filter.off);
        } else if (!any_ba && !dev.promiscuous()) {
          ba_filter = awdl::BaFilter{};
          dev.set_promiscuous(true);
        }
      }
      {
        if constexpr (caps.supports_country_notice) {
          std::string learned;
          if (dev.answer_country_notice(&learned) && learned.size() == 2 && learned != "00" && learned != have_country) {
            have_country = learned;
            log_line("event: country " + have_country); /* the service keeps two letters only */
          }
        }
        /* The channel's state as the firmware now gives it: a country taken
         * (or withdrawn) shows up here, and may_send() follows. */
        const bool open = radio::channel_active(dev.channel(), chan_flags(dev.channel()));
        if (open != channel_open) {
          channel_open = open;
          log_line(std::string("event: channel ") + std::to_string(dev.channel()) + (open ? " active" : " passive"));
        }
      }
      if (!want_country.empty() && want_country != have_country) {
        /* A peer's country (TLV 12) is applied only where the card takes one;
         * elsewhere it is dropped, as a refused one is. */
        if constexpr (caps.supports_set_country) {
          if (dev.set_country(want_country)) {
            have_country = want_country;
            log_line("event: country " + have_country);
          }
        }
        want_country.clear();
      }
      if (sta) {
        sta->tick(now_us());
        /* Against the last state published, not the one before tick(): a
         * deauthentication or rejoin changes it while frames are handled
         * (Codex review #14). */
        if (sta->state() != published_wifi) {
          published_wifi = sta->state();
          /* Before "failed", so the service knows the cause when it sees the failure (#97). */
          if (published_wifi == WifiStation::State::Failed && !sta->unsupported().empty())
            log_line("event: wifi-unsupported " + sta->unsupported());
          log_line(std::string("event: wifi ") + sta->state_name());
        }
        if constexpr (caps.supports_station) {
          std::vector<uint8_t> ans;
          while (sta->take_addba(ans)) {
            TxRate r;
            const bool prot = ans.size() > 1 && (ans[1] & 0x40) != 0;
            if (dev.sta_transmit(ans.data(), ans.size(), r, prot) < 0)
              log_line("station: failed to queue action frame");
          }
        }
        if (sta->state() != WifiStation::State::Running || sta->tx_pending()) busy = true;
      }
      if constexpr (std::is_same_v<Dev, IwxDevice>) {
        if (dev.keep_on_channel()) busy = true;
      }
      if (pump()) busy = true;
      if (inflight > 0 || !uq.empty() || !agg.wait.empty() || agg_queued() > 0) busy = true;
      if (busy) {
        idle_rounds = 0;
      } else if (++idle_rounds > 200) {
        Sleep(1); /* quiet: 1 ms ticks (timeBeginPeriod) */
      } else {
        SwitchToThread();
      }
      const uint64_t now = GetTickCount64();
      bool sta_joining = false;
      if (sta) {
        const WifiStation::State st = sta->state();
        sta_joining = st != WifiStation::State::Idle && st != WifiStation::State::Running &&
                      st != WifiStation::State::Failed;
      }
      if (opt.fast_unlock && may_send()) discovery_scans = radio::kMaxPassiveDiscoveryScans;
      const unsigned discovery_due =
          radio::passive_discovery_scan_delay_ms(opt.fast_unlock, discovery_scans, sta_joining);
      if (discovery_due && now >= discovery_from + discovery_due) {
        if (!opt.fast_unlock) {
          /* The default, unchanged: due once, whatever happens. */
          discovery_scans++;
          if (radio::should_start_passive_discovery_scan(may_send(), scan_started, scan_wanted, caps)) {
            log_line("channel " + std::to_string(dev.channel()) + " is passive: scanning once so the firmware can learn the country");
            scan_wanted = true;
            scan_ssid.clear();
          }
        } else if (may_send()) {
          discovery_scans = radio::kMaxPassiveDiscoveryScans; /* unlocked: no more scans */
        } else if (radio::should_start_passive_discovery_scan(false, scan_started, scan_wanted, caps)) {
          discovery_scans++;
          log_line("channel " + std::to_string(dev.channel()) + " is passive: fast discovery scan " +
                   std::to_string(discovery_scans) + " of " + std::to_string(radio::kMaxPassiveDiscoveryScans) +
                   " so the firmware can learn the country");
          scan_wanted = true;
          scan_ssid.clear();
          fast_discovery_wanted = true;
        }
        /* Otherwise a scan is on its way (it hears Country elements too) or
         * this backend has no firmware scan: asked again next tick. */
      }
      if (scan_wanted && !scan_started) {
        scan_wanted = false;
        fast_discovery_running = fast_discovery_wanted;
        fast_discovery_wanted = false;
        bool started = false;
        if constexpr (caps.supports_umac_scan) {
          if (fast_discovery_running)
            started = dev.umac_scan(fast_channels, fast_channel_count, nullptr, 0, dev.hw_address().data());
          else
            started = dev.umac_scan(kScanChannels, sizeof(kScanChannels), scan_ssid.data(), scan_ssid.size(),
                                    dev.hw_address().data());
        }
        scan_started_ms = now;
        scan_started = true;
        /* A fast discovery scan is not the service's Wi-Fi list: it covers 11
         * channels, and a partial list would make a pending "connect to X"
         * fail as not found (Supervisor.AfterScan). So no scan events for it. */
        if (fast_discovery_running)
          log_line(started ? "fast discovery scan started (" + std::to_string(fast_channel_count) + " channels)"
                           : std::string("fast discovery scan did not start"));
        else
          log_line(started ? "event: scan-started" : "event: scan-started (beacons on this channel only)");
      }
      bool scan_running = false;
      if constexpr (caps.supports_umac_scan) scan_running = scan_started && dev.scan_running();
      const bool scan_complete = radio::is_scan_complete(scan_started, scan_running, now - scan_started_ms, caps);
      if (scan_complete && fast_discovery_running) {
        scan_started = false;
        fast_discovery_running = false;
        log_line("fast discovery scan done; channel " + std::to_string(dev.channel()) +
                 (may_send() ? " now allows sending" : " still passive"));
      } else if (scan_complete) {
        scan_started = false;
        unsigned n = 0;
        for (const auto &[bssid, b] : bss_seen) {
          if (now - b.seen_ms > 120000 || b.ssid.empty()) continue;
          std::string hex;
          char h[3];
          for (unsigned char c : b.ssid) {
            std::snprintf(h, sizeof(h), "%02x", c);
            hex += h;
          }
          log_line("event: bss " + bssid + " " + std::to_string(b.channel) + " " + std::to_string(b.dbm) + " " +
                   b.security + " " + hex);
          n++;
        }
        log_line("event: scan-done " + std::to_string(n));
      }
      if (now - last_health >= 1000) {
        last_health = now;
        dev.check_health();
      }
      if (now - last_report >= 5000) {
        last_report = now;
        const TxStats &s = dev.tx_stats();
        bool promiscuous = true; /* without the toggle the backend never turns it off */
        if constexpr (caps.supports_promiscuous_toggle) promiscuous = dev.promiscuous();
        log_line("rx: " + std::to_string(rx_all) + " frames, " + std::to_string(rx_awdl) +
                 " AWDL action frames (" + std::to_string(rx_psf) + " PSF, " + std::to_string(rx_mif) +
                 " MIF; last " + std::to_string(awdl_dbm) + " dBm), " +
                 std::to_string(rx_to_us) + " addressed to us, " + std::to_string(rx_data) +
                 " data" +
                 (rx_fcs ? ", FCS removed " + std::to_string(rx_fcs) : "") +
                 (rx_mif_tail ? ", MIF bad tail " + std::to_string(rx_mif_tail) : "") +
                 (rx_mif_nover ? ", MIF without version " + std::to_string(rx_mif_nover) : ""));
        if constexpr (std::is_same_v<Dev, IwxDevice>) {
          const auto &ro = dev.reorder_stats();
          if (ro.held || ro.dropped_old || ro.notifications)
            log_line("reorder: " + std::to_string(ro.in_order) + " in order, " +
                     std::to_string(ro.held) + " held, " + std::to_string(ro.released) +
                     " released (" + std::to_string(ro.timed_out) + " by timeout), " +
                     std::to_string(ro.dropped_old) + " old dropped, " +
                     std::to_string(ro.notifications) + " release notifications");
        }
        if (opt.coex_plan)
          log_line(std::string("coex plan: ") + (plan.active ? "active" : "inactive") + "; " +
                   std::to_string(plan.rewritten) + " sync frames rewritten, " + std::to_string(plan.silenced) +
                   " held in router entries, " + std::to_string(plan.unaligned) + " out of step, " +
                   std::to_string(plan.stale) + " without a peer schedule; router " +
                   std::to_string(plan.meter.pps) + " pkt/s, " + std::to_string(plan.meter.avg) + " B avg");
        log_line("tx: " + std::to_string(s.submitted) + " submitted, " + std::to_string(s.acked) +
                 " done, " + std::to_string(s.failed) + " failed; unicast: " + std::to_string(resent) +
                 " resent, " + std::to_string(gave_up) + " given up, " + std::to_string(uq.size()) +
                 " waiting, " + std::to_string(uq_dropped) + " dropped, " +
                 std::to_string(missing_status) + " missing TX status; rate step " +
                 std::to_string(ladder.step) + " (" + std::to_string(ladder.ups) + " up, " +
                 std::to_string(ladder.downs) + " down); BA answers " + std::to_string(ba_answers) +
                 (ba_declined ? ", " + std::to_string(ba_declined) + " declined" : "") +
                 (promiscuous ? "" : ", promiscuous off") +
                 (tx_held_passive ? "; " + std::to_string(tx_held_passive) + " held (passive channel)" : ""));
        if constexpr (caps.supports_aggregate) if (agg.on) log_agg_snapshot();
        if (sta) {
          const auto &r = sta->rx_stats();
          const auto ws = sta->tap().write_stats();
          unsigned sta_acked = 0, sta_failed = 0, sta_submitted = 0;
          uint32_t sta_tx_ba = 0, sta_tx_rate = 0;
          if constexpr (caps.supports_station) {
            sta_acked = dev.sta_tx_acked();
            sta_failed = dev.sta_tx_failed();
            sta_submitted = dev.sta_tx_submitted();
            if constexpr (std::is_same_v<Dev, IwxDevice>) {
              sta_tx_ba = dev.sta_tx_ba_notifs();
              sta_tx_rate = dev.sta_tx_last_rate();
            }
          }
          char txr[32];
          std::snprintf(txr, sizeof(txr), "0x%08x", sta_tx_rate);
          /* "N submitted" here, no longer in the "tx:" line (#11): those
           * frames are the station's, not AWDL's. */
          log_line(std::string("wifi: ") + sta->state_name() + ", " + std::to_string(sta->rx_packets()) +
                   " packets in, " + std::to_string(sta->tx_packets()) + " out; frames to the AP " +
                   std::to_string(sta_submitted) + " submitted, " +
                   std::to_string(sta_acked) + " acknowledged, " + std::to_string(sta_failed) +
                   " not (txBA " + (sta->tx_ba_active() ? "on" : "off") + ", BAs " + std::to_string(sta_tx_ba) +
                   ", txRate " + txr + "); from the AP " + std::to_string(r.data) + " data frames (" + std::to_string(r.unicast) + " unicast), " + std::to_string(r.amsdu) +
                   " A-MSDU, dropped " + std::to_string(r.undecrypted) + " undecrypted, " +
                   std::to_string(r.not_llc) + " not LLC, " + std::to_string(r.tap_failed) + " TAP; TAP writes " +
                   std::to_string(ws.writes) + ", queue max " +
                   std::to_string(ws.queued_max) + ", drops " +
                   std::to_string(ws.drops) + ", failed " +
                   std::to_string(ws.failed) + ", write avg " +
                   std::to_string(ws.writes ? ws.total_us / ws.writes : 0) +
                   " us, max " + std::to_string(ws.max_us) + " us; TX wait " +
                   std::to_string(sta->tx_wait_queued()) + " queued, " +
                   std::to_string(sta->tx_wait_dropped()) + " dropped; RX reorder " +
                   std::to_string(sta->rx_reorder_timeouts()) + " timeouts; last rate 0x" +
                   std::to_string(r.last_rate) + " width " + std::to_string(r.last_rate_width) +
                   ", mcs " + std::to_string(r.last_rate_mcs));
          /* Stall watchdog: Windows keeps sending but nearly nothing comes
           * back while the link still looks fine (hardware, 2026-11-17:
           * after ~40 minutes, 300-600 packets a minute out against 50-100
           * in, until the station was joined again). Two minutes of that
           * and the station joins again; at most once every 10 minutes.
           * Unicast only: Windows' own multicast never gets answers. The
           * service's gateway probe is the main check; this is the fallback. */
          if (sta->state() == WifiStation::State::Running) {
            const uint64_t in = sta->rx_unicast(), out = sta->tx_unicast();
            stall_hist.push_back({in, out});
            if (stall_hist.size() > 25) stall_hist.pop_front(); /* 2 minutes of 5 s reports */
            if (stall_hist.size() == 25) {
              const uint64_t din = in - stall_hist.front().first, dout = out - stall_hist.front().second;
              /* A diagnostic only (Codex review #17): a healthy upload looks
               * the same. Recovery is the service's gateway probe ("rejoin"). */
              if (dout >= 200 && din * 10 < dout && now - last_rejoin > 600000) {
                last_rejoin = now;
                log_line("wifi: few unicast replies (" + std::to_string(dout) + " out, " + std::to_string(din) +
                         " in over 2 minutes)");
              }
            }
          } else {
            stall_hist.clear();
          }
        }
      }
    }
    if (sta) sta->stop();
    if (tap_thread.joinable()) {
      g_stop = true;
      tap_thread.join();
    }
    if (!dev.healthy()) log_line("the card stopped answering or the firmware failed");
    bool clean = true;
    if constexpr (std::is_same_v<Dev, IwxDevice>) clean = dev.stop();
    else dev.stop();
    if (!clean) {
      failed = true;
      log_line("device stop did not complete cleanly; a PnP restart is required");
    }
    };
    if (opt.kernel) {
      KernelDevice kdev(log_line);
      run(kdev);
    } else {
      IwxDevice idev(log_line);
      run(idev);
    }
    g_stop = true;
  });

  /* Wait for the card before taking clients, so HELLO tells the truth. */
  while (!ready && !failed && !stopping()) Sleep(10);

  std::vector<uint8_t> payload;
  while (!stopping() && pipe.wait_for_client()) {
    radio_link_hello hello{RADIO_LINK_VERSION, opt.channel,
                           static_cast<uint8_t>(opt.channel <= 14 ? 20 : opt.width),
                           {pipe.shm_cap_flag(), 0}};
    client_connected = true;
    pipe.send(RADIO_LINK_HELLO, {reinterpret_cast<const uint8_t *>(&hello), sizeof(hello)});
    log_line("client connected");
    radio_link_header header{};
    while (!stopping() && pipe.read_exact(&header, sizeof(header))) {
      if (header.length > RADIO_LINK_MAX_PAYLOAD) {
        log_line("client sent an oversized message");
        break;
      }
      payload.resize(header.length);
      if (header.length && !pipe.read_exact(payload.data(), header.length)) break;
      if (inbox.size() > 4096) continue; /* the card is not keeping up: drop */
      inbox.push({header.type, payload});
    }
    client_connected = false;
    {
      std::lock_guard<std::mutex> l(filter_mutex);
      have_filter = false;
    }
    log_line("client gone");
    pipe.drop_client();
  }
  g_stop = true;
  device_thread.join();
  timeEndPeriod(1);
  return failed ? 1 : 0;
}
