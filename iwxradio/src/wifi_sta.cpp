// SPDX-License-Identifier: GPL-2.0-only
/*
 * Station state machine. Frame layouts are IEEE 802.11-2016 (9.3.3, 9.4.2,
 * 12.7); the 4-way handshake follows 12.7.6: key descriptor version 2
 * (HMAC-SHA1 MIC, PRF-SHA1) for AKM 2, version 3 (AES-CMAC, KDF-SHA256) for
 * AKM 6, and AKM-defined (0) for SAE (AES-CMAC) and OWE (HMAC-SHA256-128),
 * both with KDF-SHA256. SAE is 12.4 (hostap's sae.c), OWE RFC 8110.
 */
#include "wifi_sta.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "awdl_policy.h"
#include "crypto.h"
#include "sta_mac_nat.h"

namespace {

constexpr uint16_t kEtherEapol = 0x888e;
constexpr uint8_t kRsnOui[3] = {0x00, 0x0f, 0xac};
constexpr uint64_t kStepTimeoutUS = 400000;
constexpr uint64_t kHandshakeTimeoutUS = 5000000;
constexpr uint64_t kRejoinDelayUS = 5000000;

/* Key information bits (12.7.2). */
constexpr uint16_t kKeyVersionMask = 0x0007, kKeyPairwise = 0x0008, kKeyInstall = 0x0040,
                   kKeyAck = 0x0080, kKeyMic = 0x0100, kKeySecure = 0x0200,
                   kKeyEncryptedData = 0x1000;

bool parse_mac(const std::string &s, uint8_t out[6]) {
  unsigned v[6];
  if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6 ||
      std::sscanf(s.c_str(), "%x-%x-%x-%x-%x-%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
    for (int i = 0; i < 6; i++) out[i] = static_cast<uint8_t>(v[i]);
    return true;
  }
  if (s.size() == 12) {
    for (int i = 0; i < 6; i++) {
      if (std::sscanf(s.c_str() + 2 * i, "%2x", &v[i]) != 1) return false;
      out[i] = static_cast<uint8_t>(v[i]);
    }
    return true;
  }
  return false;
}

bool valid_unicast_mac(const uint8_t m[6]) {
  if (m[0] & 1) return false; /* multicast */
  return (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) != 0;
}

bool parse_hex(const std::string &s, uint8_t *out, size_t n) {
  if (s.size() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    if (std::sscanf(s.c_str() + 2 * i, "%2x", &v) != 1) return false;
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

size_t hdrlen(const uint8_t *f, size_t len) {
  if (len < 24) return 0;
  size_t h = 24;
  if ((f[1] & 0x03) == 0x03) h += 6;
  if ((f[0] & 0x0c) == 0x08 && (f[0] & 0x80)) {
    h += 2;
    if (f[1] & 0x80) h += 4; /* +HTC: the Order bit in a QoS data frame (Codex review #26) */
  }
  return h <= len ? h : 0;
}

void put_le16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x));
  v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_be16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

/* CCMP pairwise and group with one AKM; RSN capabilities MFPC/MFPR when
 * management frames are protected. AKM 2 without PMF is the bytes nekodrop
 * always sent. */
std::vector<uint8_t> build_rsn_ie(uint8_t akm, bool pmf) {
  const uint8_t caps = pmf ? 0xc0 : 0x00; /* MFPR | MFPC */
  return {0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
          0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, akm,  caps, 0x00};
}

uint64_t le48(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 5; i >= 0; i--) v = v << 8 | p[i];
  return v;
}

TxRate mgmt_rate() {
  TxRate r;
  r.kind = TxRate::Legacy;
  r.mbps_x2 = 12; /* 6 Mb/s OFDM: valid in both bands */
  return r;
}

/* An authentication frame's header and fixed fields (9.3.3.12). */
std::vector<uint8_t> auth_frame(const uint8_t *bssid, const uint8_t *addr, uint16_t &mgmt_seq, uint16_t alg,
                                uint16_t txn, uint16_t status) {
  std::vector<uint8_t> f(24, 0);
  f[0] = 0xb0;
  std::memcpy(&f[4], bssid, 6);
  std::memcpy(&f[10], addr, 6);
  std::memcpy(&f[16], bssid, 6);
  ++mgmt_seq;
  f[22] = static_cast<uint8_t>(mgmt_seq << 4);
  f[23] = static_cast<uint8_t>(mgmt_seq >> 4);
  put_le16(f, alg);
  put_le16(f, txn);
  put_le16(f, status);
  return f;
}

} // namespace

/* ------------------------------------------------------------- config */

bool StaConfig::load(const std::string &path, std::string &error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot read " + path;
    return false;
  }
  std::string line;
  bool have_bssid = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#') continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "ssid") ssid = v;
    else if (k == "bssid") have_bssid = parse_mac(v, bssid);
    else if (k == "channel") channel = static_cast<uint8_t>(std::atoi(v.c_str()));
    else if (k == "passphrase") passphrase = v;
    else if (k == "psk") psk_hex = v;
    else if (k == "open") open = v == "1" || v == "true";
    else if (k == "tap") tap = v;
    else if (k == "width") max_width = static_cast<unsigned>(std::atoi(v.c_str()));
    else if (k == "ba") block_ack = v == "1";
    else if (k == "tap_writers") tap_writers = static_cast<unsigned>(std::atoi(v.c_str()));
    else if (k == "he") he = v == "1";
    else if (k == "mac") {
      uint8_t m[6];
      if (parse_mac(v, m) && valid_unicast_mac(m)) {
        std::memcpy(mac, m, 6);
        has_mac = true;
      }
    }
  }
  if (ssid.empty() || ssid.size() > 32) error = "ssid missing or longer than 32 bytes";
  else if (!have_bssid) error = "bssid missing";
  else if (!awdl::fixed_schedule_channel(channel) && !(channel >= 52 && channel <= 144) && channel != 165)
    error = "unsupported channel " + std::to_string(channel);
  else if (!open && psk_hex.empty() && (passphrase.size() < 8 || passphrase.size() > 63))
    error = "need a passphrase of 8-63 characters, a 64-digit psk, or open=1";
  if (!error.empty()) return false;
  return true;
}

/* -------------------------------------------------------------- start */

const char *WifiStation::state_name() const {
  switch (_state) {
  case State::Idle: return "idle";
  case State::Listening: return "waiting for a beacon";
  case State::Authenticating: return "authenticating";
  case State::Associating: return "associating";
  case State::Handshake: return "4-way handshake";
  case State::Running: return "connected";
  case State::Failed: return "failed";
  }
  return "?";
}

bool WifiStation::start(const StaConfig &cfg, std::string &error) {
  _cfg = cfg;
  if (!_cfg.open) {
    if (!_cfg.psk_hex.empty()) {
      if (!parse_hex(_cfg.psk_hex, _pmk, 32)) {
        error = "psk is not 64 hex digits";
        return false;
      }
    } else if (!crypto::pmk_from_passphrase(_cfg.passphrase, _cfg.ssid, _pmk)) {
      error = "PBKDF2 failed";
      return false;
    }
  }
  _tap.set_write_threads(_cfg.tap_writers);
  if (!_tap.is_open() && !_tap.open(_cfg.tap, error)) return false;
  _tap_mac = _tap.mac();
  if (_cfg.has_mac) std::memcpy(_addr.data(), _cfg.mac, 6);
  else _addr = _tap_mac;
  _mac_nat = (_addr != _tap_mac);
  _tap.set_connected(false);
  _akm = _cfg.open ? Akm::None : Akm::Psk;
  _pmf = false;
  _rsn_ie = rsn_ie();
  StaParams p{};
  std::memcpy(p.bssid, _cfg.bssid, 6);
  std::memcpy(p.addr, _addr.data(), 6);
  p.channel = _cfg.channel;
  p.width = 20;
  if (!_dev.sta_join(p)) {
    error = "the firmware refused the station context";
    return false;
  }
  _joins++;
  reset_join_state();
  _beacon_seen = false;
  _state = State::Listening;
  _state_since = _now;
  _tries = 0;
  char b[160];
  if (_mac_nat) {
    std::snprintf(b, sizeof(b),
                  "station: using TAP adapter \"%s\", address %02x:%02x:%02x:%02x:%02x:%02x "
                  "(TAP %02x:%02x:%02x:%02x:%02x:%02x)",
                  _tap.name().c_str(), _addr[0], _addr[1], _addr[2], _addr[3], _addr[4], _addr[5],
                  _tap_mac[0], _tap_mac[1], _tap_mac[2], _tap_mac[3], _tap_mac[4], _tap_mac[5]);
  } else {
    std::snprintf(b, sizeof(b), "station: using TAP adapter \"%s\", address %02x:%02x:%02x:%02x:%02x:%02x",
                  _tap.name().c_str(), _addr[0], _addr[1], _addr[2], _addr[3], _addr[4], _addr[5]);
  }
  _log(b);
  return true;
}

void WifiStation::stop() {
  if (_state == State::Idle) return;
  _tap.set_connected(false);
  _dev.sta_leave();
  _yield = _yield_paused = false;
  _yield_at = 0;
  _state = State::Idle;
}

/* Nothing from one join may leak into the next (Codex review #7, round 2 #8). */
void WifiStation::reset_join_state() {
  _keys_installed = _ptk_ready = _replay_valid = _rekey = _ptk_installed = _m4_unsent = false;
  _install_at = 0;
  _gtk_len = 0;
  _gtk_in_fw_valid = false;
  _last_reply.clear();
  _addba_answers.clear();
  _ba_token_valid = 0;
  _rx_reorder.clear();
  _tx_ba_state = TxBaState::Idle;
  _tx_ba_tries = 0;
  _tx_ba_last_ask = 0;
  _tx_batching = false;
  _tx_batch_dirty = false;
  _tx_wait.clear();
  _qos_seq = 0;
  _sae_step = SaeStep::None;
  _sae_token.clear();
  _sae_sc = 0;
  _sae_tokens = 0;
  _igtk_valid = _igtk_in_fw_valid = false;
  _yield = _yield_paused = false; /* sta_leave() ended any pause */
  _yield_at = 0;
}

std::vector<uint8_t> WifiStation::rsn_ie() const { return build_rsn_ie(static_cast<uint8_t>(_akm), _pmf); }

const char *WifiStation::akm_name() const {
  switch (_akm) {
  case Akm::None: return "open";
  case Akm::Psk: return _pmf ? "WPA2, protected management frames" : "WPA2";
  case Akm::PskSha256: return "WPA2-SHA256";
  case Akm::Sae: return "WPA3";
  case Akm::Owe: return "Enhanced Open";
  }
  return "?";
}

/* WPA2 first when offered (as before, and what WPA2/WPA3 mixed networks
 * accept), then PSK-SHA256, then SAE; OWE for an open network that offers it. */
std::string WifiStation::choose_akm() {
  _pmf = false;
  const auto has = [&](unsigned akm) { return (_ap_akms >> akm) & 1; };
  if (_cfg.open) {
    _akm = Akm::None;
    if (_beacon_seen && _ap_rsn && has(18)) {
      if (!_ap_pairwise_ccmp) return "the network does not offer CCMP";
      _akm = Akm::Owe;
      _pmf = true;
    }
  } else if (!_beacon_seen || !_ap_rsn) {
    _akm = Akm::Psk; /* no beacon yet: try WPA2, as always */
  } else if (!_ap_pairwise_ccmp) {
    return "the network offers only TKIP, which is not supported";
  } else if (has(2)) {
    _akm = Akm::Psk;
    _pmf = _ap_mfpr; /* only where the network requires it: mixed networks stay as before */
  } else if (has(6)) {
    _akm = Akm::PskSha256;
    _pmf = true;
  } else if (has(8)) {
    if (_cfg.passphrase.empty()) return "WPA3 needs the Wi-Fi password itself, not a stored key";
    if (_ap_h2e_only) return "the network accepts only WPA3 hash-to-element (SAE-H2E), which is not supported yet";
    _akm = Akm::Sae;
    _pmf = true;
  } else if (has(1) || has(5) || has(11) || has(12) || has(13)) {
    return "WPA2/WPA3-Enterprise (802.1X) is not supported yet";
  } else {
    return "the network's key management is not supported";
  }
  if (_pmf && !_ap_bip_cmac) return "the network protects management frames with a cipher other than BIP-CMAC-128";
  _rsn_ie = rsn_ie();
  return "";
}

/* The group key into the firmware, unless that very key is already there:
 * a reinstall resets its sequence counters for nothing (round 2 #1). */
bool WifiStation::install_gtk() {
  if (_gtk_in_fw_valid && _gtk_in_fw_idx == _gtk_index && std::memcmp(_gtk_in_fw, _gtk, 16) == 0) return true;
  if (!_dev.sta_set_key(true, _gtk_index, _gtk)) return false;
  std::memcpy(_gtk_in_fw, _gtk, 16);
  _gtk_in_fw_idx = static_cast<uint8_t>(_gtk_index);
  _gtk_in_fw_valid = true;
  return true;
}

/* The integrity group key (PMF), once per key as for the GTK. */
bool WifiStation::install_igtk() {
  if (!_pmf || !_igtk_valid) return true;
  if (_igtk_in_fw_valid && std::memcmp(_igtk_in_fw, _igtk, 16) == 0) return true;
  if (!_dev.sta_set_igtk(_igtk_id, _igtk, _igtk_ipn)) return false;
  std::memcpy(_igtk_in_fw, _igtk, 16);
  _igtk_in_fw_valid = true;
  return true;
}

void WifiStation::fail(const std::string &why) {
  _log("station: " + why + "; retrying in 5 s");
  _tap.set_connected(false);
  _dev.sta_leave();
  reset_join_state();
  _state = State::Failed;
  _state_since = _now;
}

/* -------------------------------------------------------------- demux */

bool WifiStation::owns(const RxFrame &f) const {
  if (_state == State::Idle || f.len < 24) return false;
  const uint8_t type = f.data[0] & 0x0c;
  if (type == 0x04) return false; /* control: leave to the firmware */
  const uint8_t *a1 = f.data + 4, *a2 = f.data + 10, *a3 = f.data + 16;
  if (std::memcmp(a2, _cfg.bssid, 6) == 0) return true;        /* from the AP */
  if (std::memcmp(a3, _cfg.bssid, 6) == 0 && (a1[0] & 1)) return true; /* our BSS's broadcasts */
  return false;
}

void WifiStation::on_frame(const RxFrame &f) {
  const uint8_t type = f.data[0] & 0x0c;
  if (type == 0x00) {
    handle_mgmt(f);
  } else if (type == 0x08) {
    /* A-MPDU MPDUs arrive in hardware order. Reorder only our unicast QoS
     * data while an RX Block Ack session is active; everything else takes the
     * direct path. */
    if ((f.data[4] & 1u) == 0 && std::memcmp(f.data + 4, _addr.data(), 6) == 0 &&
        _rx_reorder.process(f, _now, [this](const RxFrame &rx) { handle_data(rx); }))
      return;
    handle_data(f);
  }
}

/* ---------------------------------------------------------- management */

void WifiStation::learn_beacon(const uint8_t *f, size_t len) {
  if (len < 36) return;
  _beacon_int = le16(f + 32);
  _ap_capab = le16(f + 34);
  _ap_rsn = _ap_ht = _ap_vht = _ap_2ss = _ap_wmm = false;
  _ap_rsn_any = false;
  _ap_group_cipher = 0;
  _ap_ht_cap = 0;
  _ap_ampdu = 0;
  _ap_vht_cap = 0;
  _ap_vht_mcs = 0xffff;
  _ap_he = sta_he::ApHe{};
  _ap_pairwise_ccmp = _ap_bip_cmac = true;
  _ap_mfpc = _ap_mfpr = _ap_h2e_only = false;
  _ap_akms = 0;
  _ap_rsn_caps = 0;
  _ap_rsn_raw.clear();
  _ap_width = 20;
  _ap_rates.clear();
  for (size_t i = 36; i + 2 <= len;) {
    const uint8_t id = f[i], n = f[i + 1];
    if (i + 2 + n > len) break;
    const uint8_t *v = f + i + 2;
    if (id == 1 || id == 50) _ap_rates.insert(_ap_rates.end(), v, v + n);
    else if (id == 5 && n >= 2) _dtim = v[1] ? v[1] : 1;
    else if (id == 48 && n >= 18) {
      /* RSN (9.4.2.25): version, group cipher, pairwise ciphers, AKMs,
       * capabilities, PMKIDs, group management cipher. The group cipher
       * must be CCMP, as before. */
      _ap_rsn_raw.assign(v, v + std::min<size_t>(n, 64));
      const bool ccmp = std::memcmp(v + 2, kRsnOui, 3) == 0 && v[5] == 4;
      _ap_rsn = ccmp;
      _ap_rsn_any = true;
      _ap_group_cipher = std::memcmp(v + 2, kRsnOui, 3) == 0 ? v[5] : 0xff;
      size_t o = 6;
      const unsigned npair = le16(v + o);
      o += 2;
      _ap_pairwise_ccmp = false;
      for (unsigned k = 0; k < npair && o + 4 <= n; k++, o += 4)
        if (std::memcmp(v + o, kRsnOui, 3) == 0 && v[o + 3] == 4) _ap_pairwise_ccmp = true;
      if (o + 2 <= n) {
        const unsigned nakm = le16(v + o);
        o += 2;
        for (unsigned k = 0; k < nakm && o + 4 <= n; k++, o += 4)
          if (std::memcmp(v + o, kRsnOui, 3) == 0 && v[o + 3] < 32) _ap_akms |= 1u << v[o + 3];
      }
      if (o + 2 <= n) {
        const uint16_t caps = le16(v + o);
        o += 2;
        _ap_rsn_caps = caps;
        _ap_mfpc = caps & 0x80;
        _ap_mfpr = caps & 0x40;
      }
      if (o + 2 <= n) o += 2 + 16 * size_t(le16(v + o)); /* PMKIDs */
      if (o + 4 <= n) _ap_bip_cmac = std::memcmp(v + o, kRsnOui, 3) == 0 && v[o + 3] == 6;
    } else if (id == 45 && n >= 26) {
      _ap_ht = true;
      _ap_2ss = v[4] != 0; /* MCS 8-15 supported */
      _ap_ht_cap = le16(v);
      _ap_ampdu = v[2];
    } else if (id == 61 && n >= 2) {
      if (v[1] & 0x03) _ap_width = 40; /* secondary channel offset */
    } else if (id == 191 && n >= 12) {
      _ap_vht = true;
      _ap_vht_cap = static_cast<uint32_t>(le16(v)) | static_cast<uint32_t>(le16(v + 2)) << 16;
      _ap_vht_mcs = le16(v + 4);
    } else if (id == 192 && n >= 1) {
      if (v[0] >= 1) _ap_width = 80;
    } else if (id == 255 && n >= 1) {
      if (v[0] == sta_he::kExtHeCap) sta_he::parse_cap(v + 1, n - 1u, _ap_he);
      else if (v[0] == sta_he::kExtHeOp) sta_he::parse_op(v + 1, n - 1u, _ap_he);
      else if (v[0] == sta_he::kExtMuEdca) sta_he::parse_mu_edca(v + 1, n - 1u, _ap_he);
    } else if (id == 221 && n >= 6 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xf2 && v[3] == 0x02 &&
               (v[4] == 0x00 || v[4] == 0x01)) {
      _ap_wmm = true;
    }
    i += 2 + n;
  }
  for (uint8_t r : _ap_rates)
    if (r == 0xfb) _ap_h2e_only = true; /* BSS membership selector 123: SAE hash-to-element only */
  if (!_beacon_seen) {
    _beacon_seen = true;
    std::string sec = "open";
    if (_ap_rsn) {
      sec.clear();
      const std::pair<unsigned, const char *> names[] = {{2, "WPA2"}, {6, "WPA2-SHA256"}, {8, "WPA3"}, {18, "OWE"},
                                                         {1, "802.1X"}, {5, "802.1X-SHA256"}, {24, "WPA3-SAE-EXT"}};
      for (const auto &[akm, name] : names)
        if ((_ap_akms >> akm) & 1) sec += (sec.empty() ? "" : "/") + std::string(name);
      if (sec.empty()) sec = "RSN";
      if (_ap_mfpr) sec += ", PMF required";
      else if (_ap_mfpc) sec += ", PMF capable";
      if (_ap_h2e_only) sec += ", SAE-H2E only";
    } else if (_ap_capab & 0x10) {
      sec = "protected (not RSN-CCMP)";
    }
    char extra[96];
    std::snprintf(extra, sizeof(extra), " (cap=0x%04x ht=0x%04x ampdu=0x%02x vht=0x%08x wmm=%u)",
                  _ap_capab, _ap_ht_cap, _ap_ampdu, _ap_vht_cap, _ap_wmm ? 1u : 0u);
    _log(std::string("station: beacon heard: ") + sec +
         (_ap_ht ? ", HT" : "") + (_ap_vht ? ", VHT" : "") + (_ap_he.usable() ? ", HE" : "") + ", " + std::to_string(_ap_width) + " MHz" + extra);
  }
}

/* A probe request to the AP alone (its BSSID and SSID): the answer carries
 * what a beacon does, and a radio that hears unicast frames better than
 * broadcasts (the MT7902 beside AWDL) gets it where beacons go unheard. */
void WifiStation::send_probe() {
  std::vector<uint8_t> f(24, 0);
  f[0] = 0x40;
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], _cfg.bssid, 6);
  f[22] = static_cast<uint8_t>(++_mgmt_seq << 4);
  f[23] = static_cast<uint8_t>(_mgmt_seq >> 4);
  f.push_back(0);
  f.push_back(static_cast<uint8_t>(_cfg.ssid.size()));
  f.insert(f.end(), _cfg.ssid.begin(), _cfg.ssid.end());
  const uint8_t rates[10] = {1, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c}; /* 6-54 Mb/s */
  f.insert(f.end(), rates, rates + sizeof(rates));
  _dev.sta_transmit(f.data(), f.size(), mgmt_rate(), false);
}

void WifiStation::send_auth() {
  if (_akm == Akm::Sae) {
    if (_sae_step == SaeStep::Confirmed) sae_send_confirm();
    else sae_send_commit();
    return;
  }
  std::vector<uint8_t> f(24, 0);
  f[0] = 0xb0;
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], _cfg.bssid, 6);
  f[22] = static_cast<uint8_t>(++_mgmt_seq << 4);
  f[23] = static_cast<uint8_t>(_mgmt_seq >> 4);
  put_le16(f, 0); /* open system */
  put_le16(f, 1); /* sequence 1 */
  put_le16(f, 0); /* status */
  _dev.sta_transmit(f.data(), f.size(), mgmt_rate(), false);
}

/* SAE commit (transaction 1): derived once per join, sent again as is. */
void WifiStation::sae_send_commit() {
  if (_sae_step == SaeStep::None) {
    if (!_sae.prepare(_addr.data(), _cfg.bssid, _cfg.passphrase) || !_sae.commit())
      return fail("WPA3: could not derive the SAE password element");
    _sae_step = SaeStep::Committed;
  }
  std::vector<uint8_t> f = auth_frame(_cfg.bssid, _addr.data(), _mgmt_seq, 3, 1, 0);
  const std::vector<uint8_t> body = _sae.commit_body(_sae_token);
  f.insert(f.end(), body.begin(), body.end());
  _dev.sta_transmit(f.data(), f.size(), mgmt_rate(), false);
}

/* SAE confirm (transaction 2): send-confirm goes up with each one sent. */
void WifiStation::sae_send_confirm() {
  _sae_sc++;
  uint8_t c[32];
  if (!_sae.confirm(_sae_sc, c)) return fail("WPA3: could not compute the SAE confirm");
  std::vector<uint8_t> f = auth_frame(_cfg.bssid, _addr.data(), _mgmt_seq, 3, 2, 0);
  put_le16(f, _sae_sc);
  f.insert(f.end(), c, c + 32);
  _dev.sta_transmit(f.data(), f.size(), mgmt_rate(), false);
}

void WifiStation::handle_sae(uint16_t seq, uint16_t status, const uint8_t *body, size_t len) {
  if (seq == 1) {
    if (status == 76) { /* anti-clogging token required: commit again with it */
      if (len < 2 || ++_sae_tokens > 3) return fail("WPA3: the access point keeps asking for an anti-clogging token");
      _sae_token.assign(body + 2, body + len);
      _log("station: SAE anti-clogging token received; committing again");
      _tries = 0;
      _last_try = _now;
      return sae_send_commit();
    }
    if (status == 77) return fail("WPA3: the access point wants an SAE group other than 19 (P-256)");
    if (status == 126) return fail("WPA3: the access point requires SAE hash-to-element, which is not supported yet");
    if (status != 0) return fail("WPA3 authentication refused, status " + std::to_string(status));
    if (_sae_step == SaeStep::Committed) {
      if (!_sae.process_commit(body, len)) return fail("WPA3: the access point's SAE commit is invalid");
      _sae_step = SaeStep::Confirmed;
      _tries = 0;
      _last_try = _now;
      sae_send_confirm();
      _log("station: SAE commit exchanged, confirm sent");
    } else if (_sae_step == SaeStep::Confirmed) {
      sae_send_confirm(); /* the access point repeated its commit: our confirm was lost */
    }
  } else if (seq == 2) {
    if (status != 0) return fail("WPA3 authentication refused at confirm, status " + std::to_string(status));
    if (_sae_step != SaeStep::Confirmed || len < 2 + 32) return;
    if (!_sae.verify(le16(body), body + 2)) return fail("WPA3 authentication failed: check the Wi-Fi password");
    std::memcpy(_pmk, _sae.pmk(), 32);
    _log("station: WPA3 (SAE) authenticated");
    _state = State::Associating;
    _state_since = _now;
    _tries = 0;
    _last_try = 0;
  }
}

void WifiStation::send_assoc() {
  std::vector<uint8_t> f(24, 0);
  f[0] = 0x00;
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], _cfg.bssid, 6);
  f[22] = static_cast<uint8_t>(++_mgmt_seq << 4);
  f[23] = static_cast<uint8_t>(_mgmt_seq >> 4);
  uint16_t cap = 0x0001; /* ESS */
  if (!plain() || (_ap_capab & 0x0010)) cap |= 0x0010;      /* privacy */
  if (_ap_capab & 0x0020) cap |= 0x0020;                    /* short preamble */
  if (!_beacon_seen || (_ap_capab & 0x0400)) cap |= 0x0400; /* short slot time */
  if (_ap_capab & 0x8000) cap |= 0x8000;                    /* immediate block ack */
  put_le16(f, cap);
  put_le16(f, 10); /* listen interval */
  f.push_back(0);
  f.push_back(static_cast<uint8_t>(_cfg.ssid.size()));
  f.insert(f.end(), _cfg.ssid.begin(), _cfg.ssid.end());
  /* Filter out BSS membership selectors (HT/VHT/HE/EHT/SAE-H2E, value > 108 = 54 Mb/s)
   * so we never advertise HE/EHT membership without sending HE/EHT Capabilities IEs. */
  std::vector<uint8_t> rates;
  rates.reserve(_ap_rates.size());
  for (uint8_t r : _ap_rates)
    if ((r & 0x7f) <= 108) rates.push_back(r);
  if (rates.empty()) {
    if (_cfg.channel <= 14) rates = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c};
    else rates = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
  }
  const size_t first = std::min<size_t>(8, rates.size());
  f.push_back(1);
  f.push_back(static_cast<uint8_t>(first));
  f.insert(f.end(), rates.begin(), rates.begin() + first);
  if (rates.size() > first) {
    f.push_back(50);
    f.push_back(static_cast<uint8_t>(rates.size() - first));
    f.insert(f.end(), rates.begin() + first, rates.end());
  }
  if (!plain()) f.insert(f.end(), _rsn_ie.begin(), _rsn_ie.end());
  if (_ap_wmm) {
    /* QoS Capability IE (46, len 1, QoS Info 0), as sent by OpenBSD/mac80211. */
    const uint8_t qos_cap[3] = {46, 1, 0};
    f.insert(f.end(), qos_cap, qos_cap + sizeof(qos_cap));
  }
  if (_akm == Akm::Owe) {
    /* OWE Diffie-Hellman Parameter (extension 32): group 19, our public x. */
    const uint8_t head[5] = {255, 1 + 2 + 32, 32, 19, 0};
    f.insert(f.end(), head, head + 5);
    f.insert(f.end(), _owe.public_x(), _owe.public_x() + 32);
  }
  if (_ap_ht) {
    /* HT capabilities: 20/40 (if the AP is wide), SM PS off, short GI 20/40,
     * 1 RX STBC stream; 64 KiB A-MPDU, 8 us spacing; MCS 0-15 (two chains).
     * Keep bit 11 clear (3839-byte max A-MSDU) so frames always fit in the
     * default 4 KiB RX DMA buffers. */
    const uint16_t htcap = static_cast<uint16_t>(0x0001 | (_ap_width >= 40 ? 0x0002 : 0) | 0x000c | 0x0020 |
                                                 (_ap_width >= 40 ? 0x0040 : 0) | 0x0100);
    const uint8_t ht[28] = {45,
                            26,
                            static_cast<uint8_t>(htcap),
                            static_cast<uint8_t>(htcap >> 8),
                            0x1b,
                            0xff,
                            0xff,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0x01,
                            0,
                            0,
                            0};
    f.insert(f.end(), ht, ht + sizeof(ht));
  }
  if (_ap_vht && _cfg.channel > 14) {
    /* VHT capabilities: short GI 80, 1 RX STBC stream, 1 MiB A-MPDU;
     * MCS 0-9 on two streams. Bits 2-3 = 0b00 already advertises 80 MHz in
     * IEEE 802.11ac (0b01 would advertise 160 MHz, which this PHY does not). */
    const uint8_t vht[14] = {191, 12, 0x20, 0x01, 0x80, 0x03, 0xfa, 0xff, 0, 0, 0xfa, 0xff, 0, 0};
    f.insert(f.end(), vht, vht + sizeof(vht));
  }
  /* Experimental Wi-Fi 6: HE Capabilities only toward an HE AP that also
   * runs HT and WMM (an HE STA is a QoS HT STA), and only on an HE card. */
  _he_asked = _cfg.he && _ap_he.usable() && _ap_ht && _ap_wmm && _dev.sta_he_capable();
  if (_he_asked) {
    uint8_t he[sta_he::kOurCapElementSize];
    const size_t n = sta_he::our_cap_element(he, true);
    f.insert(f.end(), he, he + n);
  }
  if (_ap_wmm) {
    /* WMM Information Element (OUI 00-50-F2, type 2, subtype 0, version 1, QoS info 0):
     * required by 802.11n/ac when advertising HT/VHT capabilities. */
    const uint8_t wmm[9] = {221, 7, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00};
    f.insert(f.end(), wmm, wmm + sizeof(wmm));
  }
  _dev.sta_transmit(f.data(), f.size(), mgmt_rate(), false);
}

void WifiStation::handle_mgmt(const RxFrame &in) {
  RxFrame f = in;
  std::vector<uint8_t> clear;
  if (in.data[1] & 0x40) {
    /* A protected management frame (PMF): the firmware decrypted it and left
     * the CCMP header in place, as for data. Read it without. */
    constexpr uint16_t ok = kStaRxDecDone | kStaRxMicOk;
    if (!_pmf || (in.status & ok) != ok || in.len < 24 + 8) return;
    const size_t end = in.len - (in.mic_stripped ? 0 : 8);
    if (end < 24 + 8) return;
    clear.assign(in.data, in.data + 24);
    clear[1] &= static_cast<uint8_t>(~0x40);
    clear.insert(clear.end(), in.data + 32, in.data + end);
    f.data = clear.data();
    f.len = clear.size();
  }
  const uint8_t sub = f.data[0] & 0xf0;
  if (sub == 0x80 || sub == 0x50) { /* beacon, probe response */
    learn_beacon(f.data, f.len);
    return;
  }
  if (std::memcmp(f.data + 4, _addr.data(), 6) != 0) return;
  if (sub == 0xb0 && _state == State::Authenticating && f.len >= 30) {
    const uint16_t alg = le16(f.data + 24), seq = le16(f.data + 26), status = le16(f.data + 28);
    if (alg == 3 && _akm == Akm::Sae) return handle_sae(seq, status, f.data + 30, f.len - 30);
    if (alg == 0 && seq == 2) {
      if (status != 0) return fail("authentication refused, status " + std::to_string(status));
      _state = State::Associating;
      _state_since = _now;
      _tries = 0;
      _last_try = 0;
    }
  } else if ((sub == 0x10 || sub == 0x30) && _state == State::Associating && f.len >= 30) {
    const uint16_t status = le16(f.data + 26), aid = le16(f.data + 28);
    if (status == 30) return fail("association refused for now (the access point still holds our previous connection)");
    if (status != 0) {
      std::string rsn_hex;
      rsn_hex.reserve(_ap_rsn_raw.size() * 2);
      for (uint8_t b : _ap_rsn_raw) {
        char h[3];
        std::snprintf(h, sizeof(h), "%02x", b);
        rsn_hex += h;
      }
      char ctx[256];
      std::snprintf(ctx, sizeof(ctx),
                    "station: assoc context: ap_capab=0x%04x rsn_caps=0x%04x wmm=%u akm=%u pmf=%u ap_rsn=%s",
                    _ap_capab, _ap_rsn_caps, _ap_wmm ? 1u : 0u, static_cast<unsigned>(_akm), _pmf ? 1u : 0u,
                    rsn_hex.empty() ? "none" : rsn_hex.c_str());
      _log(ctx);
      if (_he_asked)
        _log("station: this request asked for Wi-Fi 6; if it keeps failing, turn off experimental Wi-Fi 6 in Settings");
      return fail("association refused, status " + std::to_string(status));
    }
    if (status == 0) {
      std::string ies;
      for (size_t i = 30; i + 2 <= f.len;) {
        const uint8_t id = f.data[i], n = f.data[i + 1];
        if (i + 2 + n > f.len) break;
        if (!ies.empty()) ies += ',';
        ies += std::to_string(id) + ":" + std::to_string(n);
        i += 2 + n;
      }
      char ab[192];
      std::snprintf(ab, sizeof(ab), "station: assoc resp cap=0x%04x aid=%u ies=[%s]",
                    le16(f.data + 24), aid & 0x3fff, ies.c_str());
      _log(ab);
    }
    if (_akm == Akm::Owe) {
      /* The AP's OWE Diffie-Hellman Parameter: group 19 and its public x. */
      const uint8_t *peer_x = nullptr;
      for (size_t i = 30; i + 2 <= f.len;) {
        const uint8_t id = f.data[i], n = f.data[i + 1];
        if (i + 2 + n > f.len) break;
        if (id == 255 && n >= 1 + 2 + 32 && f.data[i + 2] == 32 && le16(f.data + i + 3) == 19) peer_x = f.data + i + 5;
        i += 2 + n;
      }
      if (!peer_x || !_owe.derive(peer_x)) return fail("Enhanced Open: the access point's key exchange is missing or invalid");
      std::memcpy(_pmk, _owe.pmk(), 32);
    }
    unsigned width = 20;
    if (_ap_ht && _ap_width >= 40) width = std::min(_ap_width, _cfg.max_width);
    StaParams p = _dev.sta_params();
    p.ht = _ap_ht;
    p.vht = _ap_vht && _cfg.channel > 14;
    p.mimo = _ap_2ss;
    p.width = width;
    p.beacon_int = _beacon_int;
    p.ap_ht_cap = _ap_ht_cap;
    p.ap_ampdu = _ap_ampdu;
    p.ap_vht_cap = _ap_vht_cap;
    p.ap_vht_mcs = _ap_vht_mcs;
    p.he = false;
    p.ap_he = _ap_he;
    if (_he_asked) {
      /* HE only if the response carries HE Capabilities and Operation too;
       * its values win over the beacon's. */
      sta_he::ApHe resp = _ap_he;
      bool cap = false, op = false;
      for (size_t i = 30; i + 2 <= f.len;) {
        const uint8_t id = f.data[i], n = f.data[i + 1];
        if (i + 2 + n > f.len) break;
        const uint8_t *v = f.data + i + 2;
        if (id == 255 && n >= 1) {
          if (v[0] == sta_he::kExtHeCap) cap = sta_he::parse_cap(v + 1, n - 1u, resp);
          else if (v[0] == sta_he::kExtHeOp) op = sta_he::parse_op(v + 1, n - 1u, resp);
          else if (v[0] == sta_he::kExtMuEdca) sta_he::parse_mu_edca(v + 1, n - 1u, resp);
        }
        i += 2 + n;
      }
      if (cap && op) {
        p.he = true;
        p.ap_he = resp;
      }
      _log(p.he ? "station: Wi-Fi 6 (HE) agreed, BSS color " + std::to_string(resp.bss_color())
                : std::string("station: the access point answered without HE, staying on Wi-Fi 5"));
    }
    _dev.sta_join_update(p);
    if (!_dev.sta_associated(aid, _dtim)) return fail("the firmware refused the association");
    _dev.sta_rate_control();
    if (_pmf && !_dev.sta_set_mfp(true)) return fail("the firmware refused management frame protection");
    if (plain()) {
      _state = State::Running;
      _keys_installed = true;
      _tap.set_connected(true);
      _log("station: connected (open network)");
    } else {
      _state = State::Handshake;
      _replay_valid = false;
    }
    _state_since = _now;
  } else if (sub == 0xc0 || sub == 0xa0) { /* deauthentication, disassociation */
    const uint16_t reason = f.len >= 26 ? le16(f.data + 24) : 0;
    if (_state != State::Failed) fail("the access point disconnected us, reason " + std::to_string(reason));
  } else if (sub == 0xd0 && f.len >= 28 && f.data[24] == 8 && f.data[25] == 0) {
    /* SA Query request (PMF, 9.6.9): answered, protected, with its
     * transaction identifier, so the access point keeps our association. */
    if (!_pmf || !_keys_installed || !(in.data[1] & 0x40)) return;
    std::vector<uint8_t> a(28, 0);
    a[0] = 0xd0;
    a[1] = 0x40;
    std::memcpy(&a[4], _cfg.bssid, 6);
    std::memcpy(&a[10], _addr.data(), 6);
    std::memcpy(&a[16], _cfg.bssid, 6);
    ++_mgmt_seq;
    a[22] = static_cast<uint8_t>(_mgmt_seq << 4);
    a[23] = static_cast<uint8_t>(_mgmt_seq >> 4);
    a[24] = 8; /* SA Query */
    a[25] = 1; /* response */
    a[26] = f.data[26];
    a[27] = f.data[27];
    if (_addba_answers.size() < 8) _addba_answers.push_back(std::move(a));
  } else if (sub == 0xd0 && f.len >= 30 && f.data[24] == 3 && f.data[25] == 2) {
    /* DELBA (Block Ack category, action 2): the parameter set's bit 11 says
     * whether the sender started the session, bits 12-15 the TID. */
    const uint16_t params = le16(f.data + 26);
    const unsigned tid = params >> 12;
    if ((params & 0x0800) && tid < 8) {
      _dev.sta_remove_rx_ba(tid);
      _rx_reorder.on_delba(_cfg.bssid, tid, [this](const RxFrame &rx) { handle_data(rx); });
      _ba_token_valid &= static_cast<uint8_t>(~(1u << tid));
      _log("station: the access point ended Block Ack on TID " + std::to_string(tid) + ", reason " +
           std::to_string(le16(f.data + 28)));
    } else if (!(params & 0x0800) && tid == 0 && (_tx_ba_state == TxBaState::Active || _tx_ba_state == TxBaState::Asking)) {
      if (_tx_ba_state == TxBaState::Active) {
        _dev.sta_tx_agg_enable(false);
        _dev.sta_tx_agg_flush();
        _tx_ba_state = TxBaState::Idle;
        _tx_ba_tries = 0;
        _tx_ba_last_ask = 0;
      }
      _log("station: the access point ended TX Block Ack on TID 0, reason " + std::to_string(le16(f.data + 28)));
    }
  } else if (sub == 0xd0 && f.len >= 33 && f.data[24] == 3 && f.data[25] == 1) {
    /* ADDBA Response from the AP to our TX Block Ack request (TID 0). */
    awdl::AddbaAnswer ans;
    if (!awdl::parse_addba_answer(f.data, f.len, _addr.data(), &ans)) {
      _log("station: malformed ADDBA response (len " + std::to_string(f.len) + ")");
      return;
    }
    _log("station: ADDBA response tid " + std::to_string(ans.tid) + " token " + std::to_string(ans.token) +
         " status " + std::to_string(ans.status) + " imm " + std::to_string(ans.immediate ? 1 : 0) +
         " window " + std::to_string(ans.window));
    if (_tx_ba_state != TxBaState::Asking || ans.tid != 0 || ans.token == 0 || ans.token > _tx_ba_token) return;
    if (ans.status == 0 && ans.immediate && _dev.sta_tx_agg_setup() && _dev.sta_tx_agg_enable(true)) {
      _tx_ba_state = TxBaState::Active;
      _log("station: TX Block Ack on TID 0 open (window " + std::to_string(ans.window) +
           ", agg_seq " + std::to_string(_dev.sta_tx_agg_seq()) + ")");
    } else {
      _tx_ba_state = TxBaState::Failed;
      _log("station: TX Block Ack on TID 0 refused (status " + std::to_string(ans.status) + ")");
    }
  } else if (sub == 0xd0 && f.len >= 33) {
    /* Block Ack request from the AP: firmware session + answer. */
    awdl::AddbaRequest r;
    if (!awdl::parse_addba_request(f.data, f.len, _addr.data(), &r)) return;
    std::vector<uint8_t> a(awdl::kAddbaResponseLength);
    awdl::build_addba_response(a.data(), r, _addr.data(), ++_mgmt_seq);
    const unsigned tid = r.tid & 7;
    const bool same = ((_ba_token_valid >> tid) & 1) && _ba_token[tid] == r.token && _ba_ssn[tid] == r.ssn &&
                      _ba_window[tid] == r.window;
    if (_cfg.block_ack.value_or(false) && r.immediate && (same || _dev.sta_add_rx_ba(r.tid, r.ssn, r.window))) {
      /* A repeated request (same dialog token) is answered again without
       * touching the firmware's session (round 2 #12). */
      if (!same) {
        _log("station: Block Ack on TID " + std::to_string(r.tid) + " from sequence " + std::to_string(r.ssn) +
             ", window " + std::to_string(r.window));
      }
      _ba_token[tid] = r.token;
      _ba_ssn[tid] = r.ssn;
      _ba_window[tid] = r.window;
      _ba_token_valid |= static_cast<uint8_t>(1u << tid);
      if (!same) {
        /* New dialog: start the scoreboard at the AP's starting sequence. A
         * repeated request keeps its buffered MPDUs. */
        _rx_reorder.on_addba(_cfg.bssid, tid, r.ssn, r.window, _now,
                             [this](const RxFrame &rx) { handle_data(rx); });
      }
    } else {
      /* Declined: status 37 (request declined); the AP sends unaggregated.
       * Any session left for this TID goes too (round 2 #13). */
      a[27] = 37;
      a[28] = 0;
      _dev.sta_remove_rx_ba(r.tid);
      _rx_reorder.on_delba(_cfg.bssid, r.tid, [this](const RxFrame &rx) { handle_data(rx); });
      _ba_token_valid &= static_cast<uint8_t>(~(1u << tid));
      if (_declined_ba++ < 3) _log("station: Block Ack request on TID " + std::to_string(r.tid) + " declined");
    }
    /* Block Ack action frames are robust: under PMF the answer goes out protected. */
    if (_pmf && _keys_installed) a[1] |= 0x40;
    if (_addba_answers.size() < 8) _addba_answers.push_back(std::move(a));
  }
}

bool WifiStation::take_addba(std::vector<uint8_t> &answer) {
  if (_addba_answers.empty()) return false;
  answer = std::move(_addba_answers.front());
  _addba_answers.pop_front();
  return true;
}

/* ---------------------------------------------------------------- data */

void WifiStation::handle_data(const RxFrame &f) {
  const uint8_t flags = f.data[1];
  if ((flags & 0x03) != 0x02) return;        /* FromDS only */
  if (f.data[0] & 0x40) return;               /* null / QoS null */
  const size_t h = hdrlen(f.data, f.len);
  if (!h) return;
  if ((flags & 0x04) != 0 || (f.data[22] & 0x0f) != 0) {
    if (_rx.not_llc++ < 5) _log("station: fragmented data frame dropped");
    return;
  }
  /* The AWDL listener is promiscuous on the same channel: the AP's frames
   * to other stations arrive too, sealed with their keys. Only ours and the
   * BSS's group frames count. */
  const bool group = (f.data[4] & 1) != 0;
  if (!group && std::memcmp(f.data + 4, _addr.data(), 6) != 0) return;
  _rx.data++;
  if (!group) _rx.unicast++;
  _rx.last_rate = f.rate_n_flags;
  _rx.last_rate_width = static_cast<uint8_t>((f.rate_n_flags >> 11) & 0x7);
  _rx.last_rate_mcs = static_cast<uint8_t>(f.rate_n_flags & 0x0f);
  const uint8_t *da = f.data + 4, *sa = f.data + 16;
  if (std::memcmp(sa, _addr.data(), 6) == 0) return; /* our own broadcast, echoed */
  const uint8_t *body = f.data + h;
  size_t len = f.len - h;
  const bool encrypted = (flags & 0x40) != 0;
  if (encrypted) {
    /* Protected: the firmware decrypted it, or it is dropped. */
    if ((f.status & (kStaRxDecDone | kStaRxMicOk)) !=
        (kStaRxDecDone | kStaRxMicOk)) {
      if (_rx.undecrypted++ < 5) {
        char b[96];
        std::snprintf(b, sizeof(b), "station: frame not decrypted (status 0x%04x, %zu bytes)", f.status, f.len);
        _log(b);
      }
      return;
    }
    if (len < 8) return;
    body += 8; /* CCMP header stays in place */
    len -= 8;
    if (!f.mic_stripped) {
      if (len < 8) return;
      len -= 8;
    }
  } else if (!plain()) {
    /* Unencrypted frames on a protected network are valid only during the
     * initial 4-way handshake before pairwise keys are installed. */
    if (_state != State::Handshake || _keys_installed) {
      if (_rx.undecrypted++ < 5) _log("station: unencrypted frame dropped on protected network");
      return;
    }
  }
  /* A-MSDU (QoS control bit 7): the payload is subframes, each DA, SA,
   * length, then LLC/SNAP, padded to 4 bytes. The hardware usually splits
   * them into one subframe per MPDU, keeping the bit (iwx_rx_mpdu_mq); a
   * whole A-MSDU takes the same loop. Without this, an access point that
   * turns A-MSDU on after a while starves the station of everything but
   * broadcasts (hardware, 2026-11-17). */
  const bool qos = (f.data[0] & 0x88) == 0x88;
  const size_t qos_at = (flags & 0x03) == 0x03 ? 30 : 24;
  if (qos && qos_at < h && (f.data[qos_at] & 0x80)) {
    if (!encrypted && !plain()) {
      if (_rx.not_llc++ < 5) _log("station: unencrypted A-MSDU dropped");
      return;
    }
    _rx.amsdu++;
    while (len >= 14 + 8) {
      const size_t sub = be16(body + 12);
      if (sub < 8 || 14 + sub > len) {
        if (_rx.not_llc++ < 5) _log("station: malformed A-MSDU subframe dropped");
        return;
      }
      deliver(body, body + 6, body + 14, sub, encrypted);
      const size_t step = (14 + sub + 3) & ~size_t(3);
      if (step >= len) break;
      body += step;
      len -= step;
    }
    return;
  }
  deliver(da, sa, body, len, encrypted);
}

/* One LLC/SNAP payload from the AP: EAPOL to the handshake, the rest to
 * Windows as an Ethernet frame. */
void WifiStation::deliver(const uint8_t *da, const uint8_t *sa, const uint8_t *body, size_t len, bool encrypted) {
  if (len < 8 || body[0] != 0xaa || body[1] != 0xaa || body[2] != 0x03) {
    if (_rx.not_llc++ < 5) {
      char b[128];
      std::snprintf(b, sizeof(b), "station: data frame without LLC/SNAP dropped (%zu bytes: %02x %02x %02x %02x)",
                    len, len > 0 ? body[0] : 0, len > 1 ? body[1] : 0, len > 2 ? body[2] : 0, len > 3 ? body[3] : 0);
      _log(b);
    }
    return;
  }
  if (std::memcmp(sa, _addr.data(), 6) == 0) return; /* our own broadcast, echoed */
  const uint16_t ethertype = be16(body + 6);
  body += 8;
  len -= 8;
  if (ethertype == kEtherEapol) {
    handle_eapol(body, len, encrypted);
    return;
  }
  if (!plain() && !encrypted) return;
  if (_state != State::Running) return;
  const uint8_t *tap_da = da;
  std::vector<uint8_t> translated;
  if (_mac_nat) {
    if (std::memcmp(da, _addr.data(), 6) == 0) tap_da = _tap_mac.data();
    const bool maybe_ctrl =
        ethertype == 0x0806 ||
        (ethertype == 0x0800 && sta_mac_nat::payload_is_dhcp(body, len, false)) ||
        (ethertype == 0x86dd && len >= 48 && body[6] == 58);
    if (maybe_ctrl) {
      translated.assign(body, body + len);
      if (sta_mac_nat::translate_payload(ethertype, translated.data(), translated.size(),
                                         _addr.data(), _tap_mac.data(), false)) {
        body = translated.data();
      }
    }
  }
  if (_tap.write_eth(tap_da, sa, ethertype, body, len)) {
    _rx_packets++;
    _rx_bytes += 14 + len;
  } else if (_rx.tap_failed++ < 5) {
    _log("station: writing to the TAP adapter failed");
  }
}

bool WifiStation::transmit_qos_agg(const uint8_t *da, uint16_t ethertype, const uint8_t *payload, size_t len,
                                   bool protect, bool kick) {
  _tx_frame.assign(26, 0);
  auto &f = _tx_frame;
  f[0] = 0x88; /* QoS Data */
  f[1] = static_cast<uint8_t>(0x01 | (protect ? 0x40 : 0)); /* ToDS */
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], da, 6);
  const uint16_t sc = static_cast<uint16_t>(_dev.sta_tx_agg_seq() << 4);
  f[22] = static_cast<uint8_t>(sc);
  f[23] = static_cast<uint8_t>(sc >> 8);
  f[24] = 0; /* TID 0, normal ACK */
  f[25] = 0;
  const uint8_t llc[6] = {0xaa, 0xaa, 0x03, 0, 0, 0};
  f.insert(f.end(), llc, llc + 6);
  put_be16(f, ethertype);
  f.insert(f.end(), payload, payload + len);
  if (_dev.sta_transmit_agg(f.data(), f.size(), protect, kick) < 0) return false;
  _tx_packets++;
  _tx_bytes += f.size();
  return true;
}

bool WifiStation::flush_tx() {
  bool sent_any = false;
  if (!sta_tx::can_send(_state == State::Running, _yield, _yield_paused)) {
    _tx_batch_dirty = false;
    _tx_batching = false;
    return false;
  }
  if (_tx_ba_state == TxBaState::Active && _dev.sta_tx_agg_ready()) {
    const unsigned cap = _dev.sta_tx_agg_cap();
    unsigned batch = 0;
    while (!_tx_wait.empty() && _dev.sta_tx_agg_queued() < cap) {
      const PendingEth &p = _tx_wait.front();
      if (p.frame.size() < 14) {
        _tx_wait.pop_front();
        continue;
      }
      const uint16_t ethertype = be16(p.frame.data() + 12);
      if (!transmit_qos_agg(p.frame.data(), ethertype, p.frame.data() + 14, p.frame.size() - 14, p.protect, false))
        break;
      _tx_wait.pop_front();
      _tx_batch_dirty = true;
      sent_any = true;
      if (++batch >= 32) {
        _dev.sta_kick_tx_agg();
        _tx_batch_dirty = false;
        batch = 0;
      }
    }
    if (_tx_batch_dirty) {
      _dev.sta_kick_tx_agg();
      _tx_batch_dirty = false;
    }
  } else {
    /* TX Block Ack is gone (DELBA, a failed negotiation, or the firmware
     * queue disabled): do not leave frames stranded in _tx_wait. Send them
     * in order on the normal path instead. The old code only cleared the
     * batch flag here, so a non-empty queue kept tx_pending() true forever
     * and made the radio main loop spin. */
    _tx_batch_dirty = false;
    while (!_tx_wait.empty()) {
      PendingEth p = std::move(_tx_wait.front());
      _tx_wait.pop_front();
      if (p.frame.size() < 14) continue;
      const uint16_t ethertype = be16(p.frame.data() + 12);
      if (!send_data(p.frame.data(), ethertype, p.frame.data() + 14, p.frame.size() - 14, p.protect)) {
        _tx_wait.push_front(std::move(p));
        break;
      }
      sent_any = true;
    }
  }
  _tx_batching = false;
  return sent_any;
}

bool WifiStation::send_data(const uint8_t *da, uint16_t ethertype, const uint8_t *payload, size_t len,
                            bool protect) {
  const bool qos = _cfg.block_ack.value_or(false) && _ap_wmm && (_ap_ht || _ap_vht) &&
                   ethertype != kEtherEapol && !(da[0] & 1);
  _tx_frame.assign(qos ? 26 : 24, 0);
  auto &f = _tx_frame;
  f[0] = qos ? 0x88 : 0x08;
  f[1] = static_cast<uint8_t>(0x01 | (protect ? 0x40 : 0)); /* ToDS */
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], da, 6);
  if (qos) {
    const uint16_t sc = static_cast<uint16_t>((++_qos_seq) << 4);
    f[22] = static_cast<uint8_t>(sc);
    f[23] = static_cast<uint8_t>(sc >> 8);
    f[24] = 0; /* TID 0, normal ACK */
    f[25] = 0;
  }
  const uint8_t llc[6] = {0xaa, 0xaa, 0x03, 0, 0, 0};
  f.insert(f.end(), llc, llc + 6);
  put_be16(f, ethertype);
  f.insert(f.end(), payload, payload + len);
  TxRate rate;
  if (ethertype == kEtherEapol) {
    rate = mgmt_rate();
  } else if (_dev.sta_tlc()) {
    rate.kind = TxRate::Auto;
  } else if (_ap_ht) {
    rate.kind = TxRate::Ht;
    rate.mcs = _ap_2ss ? 12 : 7; /* 2x2 MCS 12 or 1x1 MCS 7 */
  } else {
    rate.kind = TxRate::Legacy;
    rate.mbps_x2 = 108; /* 54 Mb/s */
  }
  if (_dev.sta_transmit(f.data(), f.size(), rate, protect) < 0) return false;
  _tx_packets++;
  _tx_bytes += f.size();
  return true;
}

void WifiStation::send_null(bool power_save) {
  std::vector<uint8_t> f(24, 0);
  f[0] = 0x48;                                                     /* null data */
  f[1] = static_cast<uint8_t>(0x01 | (power_save ? 0x10 : 0)); /* ToDS, PM */
  std::memcpy(&f[4], _cfg.bssid, 6);
  std::memcpy(&f[10], _addr.data(), 6);
  std::memcpy(&f[16], _cfg.bssid, 6);
  _dev.sta_transmit(f.data(), f.size(), mgmt_rate(), false);
}

bool WifiStation::set_yield(bool on) {
  if (on == _yield) return true;
  if (on) {
    if (_state != State::Running || _yield_unsupported) return false;
    send_null(true);
    _yield = true;
    _yield_at = _now + 30000; /* the null frame's turn on the air */
    return true;
  }
  _yield = false;
  _yield_at = 0;
  if (_yield_paused) {
    _yield_paused = false;
    if (!_dev.sta_pause(false)) {
      fail("the firmware did not bring the station link back");
      return false;
    }
  }
  if (_state == State::Running) send_null(false);
  return true;
}

void WifiStation::on_ethernet(const uint8_t *frame, size_t len) {
  if (_state != State::Running || len < 14) return;
  if (_yield) {
    _yield_dropped++;
    return;
  }
  /* The adapter's own address is ours; anything else would be bridged
   * traffic, which a station may not send. */
  const bool from_tap = std::memcmp(frame + 6, _tap_mac.data(), 6) == 0;
  const bool from_air = std::memcmp(frame + 6, _addr.data(), 6) == 0;
  if (!from_tap && !from_air) return;
  const uint16_t ethertype = be16(frame + 12);
  const bool unicast = !(frame[0] & 1);
  if (unicast) _tx_unicast++;
  const bool protect = _keys_installed && !plain();
  _tx_translated.clear();
  const uint8_t *payload = frame + 14;
  const size_t payload_len = len - 14;
  if (_mac_nat && (ethertype == 0x0806 ||
                   (ethertype == 0x0800 && sta_mac_nat::payload_is_dhcp(payload, payload_len, true)) ||
                   (ethertype == 0x86dd && payload_len >= 48 && payload[6] == 58))) {
    _tx_translated.assign(frame, frame + len);
    std::memcpy(_tx_translated.data() + 6, _addr.data(), 6);
    sta_mac_nat::translate_payload(ethertype, _tx_translated.data() + 14, payload_len,
                                   _tap_mac.data(), _addr.data(), true);
    frame = _tx_translated.data();
    payload = _tx_translated.data() + 14;
  }
  if (unicast && ethertype != kEtherEapol && _tx_ba_state == TxBaState::Active && _dev.sta_tx_agg_ready()) {
    const unsigned cap = _dev.sta_tx_agg_cap();
    if (_tx_wait.empty() && _dev.sta_tx_agg_queued() < cap &&
        transmit_qos_agg(frame, ethertype, payload, payload_len, protect, !_tx_batching)) {
      if (_tx_batching) _tx_batch_dirty = true;
      return;
    }
    if (sta_tx::host_queue_full(static_cast<unsigned>(_tx_wait.size()))) {
      _tx_wait_dropped++;
    } else {
      _tx_wait.push_back({std::vector<uint8_t>(frame, frame + len), protect});
    }
    return;
  }
  send_data(frame, ethertype, payload, payload_len, protect);
}

/* ---------------------------------------------------------------- WPA2 */

bool WifiStation::derive_ptk(const uint8_t anonce[32], uint8_t out[48]) {
  uint8_t data[6 + 6 + 32 + 32];
  const bool ap_low = std::memcmp(_cfg.bssid, _addr.data(), 6) < 0;
  std::memcpy(data, ap_low ? _cfg.bssid : _addr.data(), 6);
  std::memcpy(data + 6, ap_low ? _addr.data() : _cfg.bssid, 6);
  const bool a_low = std::memcmp(anonce, _snonce, 32) < 0;
  std::memcpy(data + 12, a_low ? anonce : _snonce, 32);
  std::memcpy(data + 44, a_low ? _snonce : anonce, 32);
  if (_akm == Akm::Psk) return crypto::prf_sha1(_pmk, 32, "Pairwise key expansion", data, sizeof(data), out, 48);
  return wpa3::kdf_sha256(_pmk, 32, "Pairwise key expansion", data, sizeof(data), out, 384);
}

/* EAPOL-Key layout (12.7.2): version, type 3, length; descriptor 2; key info
 * at 5; key length 7; replay 9; nonce 17; IV 49; RSC 65; reserved 73; MIC 81;
 * data length 97; data 99. */
bool WifiStation::take_gtk(const std::vector<uint8_t> &plain) {
  for (size_t i = 0; i + 2 <= plain.size();) {
    const uint8_t id = plain[i], n = plain[i + 1];
    if (id == 0xdd && n == 0) break; /* padding */
    if (i + 2 + n > plain.size()) break;
    /* GTK KDE: OUI 00-0F-AC, type 1, key ID byte, reserved, key. CCMP: 16 bytes. */
    if (id == 0xdd && n == 6 + 16 && std::memcmp(&plain[i + 2], kRsnOui, 3) == 0 && plain[i + 5] == 1) {
      _gtk_index = plain[i + 6] & 3;
      _gtk_len = 16;
      std::memcpy(_gtk, &plain[i + 8], 16);
      return true;
    }
    i += 2 + n;
  }
  return false;
}

/* IGTK KDE: OUI 00-0F-AC, type 9, key ID (2), IPN (6), key (16). */
void WifiStation::take_igtk(const std::vector<uint8_t> &plain) {
  for (size_t i = 0; i + 2 <= plain.size();) {
    const uint8_t id = plain[i], n = plain[i + 1];
    if (id == 0xdd && n == 0) break;
    if (i + 2 + n > plain.size()) break;
    if (id == 0xdd && n == 4 + 2 + 6 + 16 && std::memcmp(&plain[i + 2], kRsnOui, 3) == 0 && plain[i + 5] == 9) {
      const unsigned key_id = le16(&plain[i + 6]);
      if (key_id == 4 || key_id == 5) {
        _igtk_id = key_id;
        _igtk_ipn = le48(&plain[i + 8]);
        std::memcpy(_igtk, &plain[i + 14], 16);
        _igtk_valid = true;
      }
      return;
    }
    i += 2 + n;
  }
}

/* The EAPOL-Key MIC for the AKM in use (12.7.3): HMAC-SHA1 (AKM 2),
 * AES-CMAC (AKM 6, SAE), HMAC-SHA256 truncated (OWE, group 19). */
bool WifiStation::compute_mic(const uint8_t *key, const uint8_t *data, size_t len, uint8_t out[16]) const {
  if (_akm == Akm::PskSha256 || _akm == Akm::Sae) return wpa3::aes_cmac(key, data, len, out);
  uint8_t mac[32];
  const bool ok = _akm == Akm::Owe ? crypto::hmac_sha256(key, 16, data, len, mac) : crypto::hmac_sha1(key, 16, data, len, mac);
  std::memcpy(out, mac, 16);
  return ok;
}

bool WifiStation::mic_ok(const uint8_t *eapol, size_t len, const uint8_t *key) {
  if (len < 99) return false;
  std::vector<uint8_t> copy(eapol, eapol + len);
  std::memset(copy.data() + 81, 0, 16);
  uint8_t mac[16];
  if (!compute_mic(key ? key : kck(), copy.data(), copy.size(), mac)) return false;
  return std::memcmp(mac, eapol + 81, 16) == 0;
}

void WifiStation::add_mic(std::vector<uint8_t> &eapol, const uint8_t *key) {
  std::memset(eapol.data() + 81, 0, 16);
  uint8_t mac[16];
  compute_mic(key ? key : kck(), eapol.data(), eapol.size(), mac);
  std::memcpy(eapol.data() + 81, mac, 16);
}

bool WifiStation::send_eapol(const std::vector<uint8_t> &eapol, bool protect) {
  return send_data(_cfg.bssid, kEtherEapol, eapol.data(), eapol.size(), protect);
}

void WifiStation::handle_eapol(const uint8_t *body, size_t len, bool encrypted) {
  if (plain() || len < 99 || body[1] != 3 || body[4] != 2) return;
  /* Only while a handshake can be going on: a late message after a failed
   * join must not refill what fail() cleared (round 2 #8). */
  if (_state != State::Handshake && _state != State::Running) return;
  if (_keys_installed && !encrypted) {
    _log("station: unencrypted EAPOL frame dropped after key installation");
    return;
  }
  const size_t total = 4 + be16(body + 2);
  if (total > len || total < 99) return;
  const uint16_t info = be16(body + 5);
  if ((info & kKeyVersionMask) != key_version()) {
    _log("station: unexpected EAPOL key version " + std::to_string(info & kKeyVersionMask) + " for " + akm_name());
    return;
  }
  const uint8_t *replay = body + 9;
  if (_replay_valid && std::memcmp(replay, _replay, 8) <= 0) {
    /* The AP sends a message again when our reply was lost: answer with the
     * same reply, without new nonces or keys (Codex review #3). */
    if (!_last_reply.empty() && std::memcmp(replay, _last_reply_replay, 8) == 0) {
      /* Only for a genuine repeat (round 2 #4): message 1 carries no MIC but
       * must bring the same ANonce; the others must pass the MIC. */
      const bool m1 = (info & kKeyPairwise) && (info & kKeyAck) && !(info & kKeyMic);
      const bool genuine = m1 ? std::memcmp(body + 17, _anonce, 32) == 0
                              : (mic_ok(body, total, _ptk) || (_rekey && mic_ok(body, total, _tptk)));
      if (genuine && send_eapol(_last_reply, _last_reply_protect)) {
        _log("station: EAPOL message repeated by the access point; reply sent again");
        /* The 4/4 that could not be queued the first time has now left:
         * its keys go in as they would have then (round 3 #1). */
        if (_m4_unsent) {
          _m4_unsent = false;
          _install_at = _now + 20000;
        }
      }
    }
    return;
  }
  const uint16_t data_len = be16(body + 97);
  if (99u + data_len > total) return;
  const uint8_t *key_data = body + 99;

  auto remember = [&](const std::vector<uint8_t> &reply, bool protect) {
    _last_reply = reply;
    _last_reply_protect = protect;
    std::memcpy(_last_reply_replay, replay, 8);
  };
  auto make_reply = [&](uint16_t reply_info, const std::vector<uint8_t> &kd) {
    std::vector<uint8_t> e(99, 0);
    e[0] = body[0]; /* the AP's EAPOL version */
    e[1] = 3;
    const uint16_t blen = static_cast<uint16_t>(95 + kd.size());
    e[2] = static_cast<uint8_t>(blen >> 8);
    e[3] = static_cast<uint8_t>(blen);
    e[4] = 2;
    e[5] = static_cast<uint8_t>(reply_info >> 8);
    e[6] = static_cast<uint8_t>(reply_info);
    std::memcpy(&e[9], replay, 8);
    e[97] = static_cast<uint8_t>(kd.size() >> 8);
    e[98] = static_cast<uint8_t>(kd.size());
    e.insert(e.end(), kd.begin(), kd.end());
    return e;
  };

  if ((info & kKeyPairwise) && (info & kKeyAck) && !(info & kKeyMic)) {
    /* Message 1: ANonce. */
    /* While connected this is a PTK rekey: the new key is derived beside
     * the one in use, which stays installed until message 4 has left
     * (Codex review #2: installing only in the Handshake state left the
     * firmware on the old key while the AP moved to the new one). */
    _rekey = _keys_installed;
    _rekey_since = _now;
    _install_at = 0;
    _ptk_installed = _m4_unsent = false;
    std::memcpy(_anonce, body + 17, 32);
    crypto::random_bytes(_snonce, 32);
    if (!derive_ptk(_anonce, _rekey ? _tptk : _ptk)) return;
    _ptk_ready = true;
    std::memcpy(_replay, replay, 8);
    _replay_valid = true;
    std::vector<uint8_t> m2 = make_reply(key_version() | kKeyPairwise | kKeyMic, _rsn_ie);
    std::memcpy(&m2[17], _snonce, 32);
    add_mic(m2);
    const bool protect = _keys_installed;
    send_eapol(m2, protect);
    remember(m2, protect);
    _log(std::string("station: 4-way handshake 1/4 received, 2/4 sent") + (_rekey ? " (rekey)" : ""));
  } else if ((info & kKeyPairwise) && (info & kKeyAck) && (info & kKeyMic) && (info & kKeyInstall)) {
    /* Message 3: verify, unwrap the GTK, answer, install. */
    if (!_ptk_ready || std::memcmp(body + 17, _anonce, 32) != 0) return;
    if (_ptk_installed) {
      /* The AP repeats message 3 with a higher counter when our 4/4 got
       * lost. Answer, but never install the same PTK again: that resets our
       * packet numbers and the AP would drop everything we send as replays
       * (round 2 #1). */
      if (!mic_ok(body, total)) return;
      std::memcpy(_replay, replay, 8);
      std::vector<uint8_t> again = make_reply(key_version() | kKeyPairwise | kKeyMic | kKeySecure, {});
      add_mic(again);
      send_eapol(again, true);
      remember(again, true);
      _log("station: message 3/4 repeated; 4/4 sent again");
      return;
    }
    if (!mic_ok(body, total)) {
      _log("station: message 3/4 has a bad MIC (wrong password?)");
      return fail("4-way handshake failed: check the Wi-Fi password");
    }
    std::memcpy(_replay, replay, 8);
    if ((info & kKeyEncryptedData) && data_len >= 24) {
      std::vector<uint8_t> plain;
      if (!crypto::aes_unwrap(kck() + 16, 16, key_data, data_len, plain))
        return fail("could not unwrap the group key");
      if (!take_gtk(plain)) return fail("message 3/4 carries no usable group key");
      take_igtk(plain);
      if (_pmf && !_igtk_valid) _log("station: message 3/4 carries no integrity group key");
    }
    std::vector<uint8_t> m4 = make_reply(key_version() | kKeyPairwise | kKeyMic | kKeySecure, {});
    add_mic(m4);
    /* During a rekey message 4 goes out under the key in use. */
    const bool protect = _keys_installed;
    remember(m4, protect);
    if (!send_eapol(m4, protect)) {
      /* Not even queued (round 2 #6): no keys yet; the AP repeats 3/4. */
      _m4_unsent = true;
      _log("station: message 4/4 could not be queued; waiting for 3/4 again");
      return;
    }
    /* Keys go in once message 4 has left: installing first would make the
     * firmware encrypt it. */
    _install_at = _now + 20000;
    _log("station: 4-way handshake 3/4 received, 4/4 sent");
  } else if (!(info & kKeyPairwise) && (info & kKeyAck) && (info & kKeyMic)) {
    /* Group key handshake (rekey). */
    /* During a pairwise rekey the AP's group message waits: it repeats it
     * once the new PTK is in (round 2 #5). Otherwise the PTK in use signs,
     * verifies and unwraps, one context throughout. */
    if (!_keys_installed || _rekey || !mic_ok(body, total, _ptk)) return;
    std::memcpy(_replay, replay, 8);
    /* The AP is told only once the new group key is in the firmware
     * (Codex review #5): acknowledging a key we could not install would
     * silently cut broadcasts (ARP, DHCP). */
    std::vector<uint8_t> plain;
    if (!(info & kKeyEncryptedData) || !crypto::aes_unwrap(_ptk + 16, 16, key_data, data_len, plain) ||
        !take_gtk(plain))
      return fail("the group key update could not be read");
    take_igtk(plain);
    if (!install_gtk()) return fail("the firmware refused the new group key");
    if (!install_igtk()) return fail("the firmware refused the new integrity group key");
    std::vector<uint8_t> g2 = make_reply(key_version() | kKeyMic | kKeySecure, {});
    add_mic(g2, _ptk);
    send_eapol(g2, true);
    remember(g2, true);
    _log("station: group key renewed");
  }
}

/* --------------------------------------------------------------- timers */

void WifiStation::tick(uint64_t now_us) {
  _now = now_us;
  /* Release a burst whose missing MPDU never arrived after the 40 ms window. */
  _rx_reorder.flush_expired(now_us, [this](const RxFrame &rx) { handle_data(rx); });
  /* First tick after start() before _now was populated: anchor _state_since so
   * Listening genuinely waits for a beacon instead of timing out immediately. */
  if (_state != State::Idle && _state_since == 0) _state_since = now_us;
  if (_yield && !_yield_paused && _yield_at && now_us >= _yield_at) {
    _yield_at = 0;
    if (_state == State::Running && _dev.sta_pause(true)) {
      _yield_paused = true;
    } else {
      _yield = false;
      _yield_unsupported = true;
      _log("station: the link cannot be paused here; AWDL keeps sharing the radio with it");
      if (_state == State::Running) send_null(false);
    }
  }
  /* Keys go in once message 4 has left, for the first handshake and for a
   * rekey alike (Codex review #2: rekeys used to be acknowledged and never
   * installed). */
  if (_install_at && now_us >= _install_at && (_state == State::Handshake || _state == State::Running)) {
    _install_at = 0;
    const bool rekey = _rekey;
    if (rekey) {
      std::memcpy(_ptk, _tptk, sizeof(_ptk));
      _rekey = false;
    }
    if (!_dev.sta_set_key(false, 0, _ptk + 32)) return fail("could not install the pairwise key");
    _ptk_installed = true;
    if (_gtk_len && !install_gtk()) return fail("could not install the group key");
    if (!install_igtk()) return fail("could not install the integrity group key");
    _keys_installed = true;
    if (rekey) {
      _log("station: pairwise key renewed");
    } else {
      _state = State::Running;
      _state_since = now_us;
      _tap.set_connected(true);
      _log("station: connected to \"" + _cfg.ssid + "\" (" + akm_name() + ")");
    }
    return;
  }
  /* A rekey whose message 3 never came is given up; the PTK in use stays
   * (round 2 #9). The AP starts again or disconnects us, which rejoins. */
  if (_rekey && !_install_at && now_us - _rekey_since > kHandshakeTimeoutUS) {
    _rekey = false;
    _log("station: pairwise rekey did not finish; keeping the current key");
  }
  switch (_state) {
  case State::Listening:
    /* A beacon tells us the AP's capabilities (without one the association
     * asks for neither HT nor VHT: 54 Mb/s at most), or the answer to a
     * probe, asked every 300 ms; after 3 s go anyway. */
    if (!_beacon_seen && _cfg.channel > 14 && now_us - _state_since > 300000 &&
        (!_last_try || now_us - _last_try > 300000)) {
      _last_try = now_us;
      send_probe();
    }
    if (_beacon_seen || now_us - _state_since > 3000000) {
      if (!_cfg.open && _beacon_seen && !_ap_rsn && (_ap_capab & 0x10)) {
        /* Say which, for the service and the user (#97): a WPA/WPA2 mixed
         * router keeps TKIP as its group cipher although clients use CCMP. */
        if (_ap_rsn_any && _ap_group_cipher == 2 && _ap_pairwise_ccmp) {
          _unsupported = "tkip-group";
          fail("the network is WPA/WPA2 mixed (CCMP for us, TKIP for broadcasts); only CCMP is supported");
        } else if (_ap_rsn_any) {
          _unsupported = "no-ccmp";
          fail("the network offers TKIP only; only CCMP is supported");
        } else {
          _unsupported = "wep-or-wpa";
          fail("the network uses WEP or WPA (version 1), which are not supported");
        }
        return;
      }
      const std::string why = choose_akm();
      if (!why.empty()) return fail(why);
      if (_akm == Akm::Owe && !_owe.generate()) return fail("Enhanced Open: could not make a key pair");
      if ((_akm != Akm::Psk && _akm != Akm::None) || _pmf) _log(std::string("station: joining with ") + akm_name());
      _state = State::Authenticating;
      _state_since = now_us;
      _tries = 0;
      _last_try = 0;
    }
    break;
  case State::Authenticating:
  case State::Associating:
    if (!_last_try || now_us - _last_try > kStepTimeoutUS) {
      if (++_tries > 6) {
        /* Say when the AP was never heard: the BSSID is likely gone (#27). */
        fail(std::string(_state == State::Authenticating ? "no answer to authentication" : "no answer to association") +
             (_beacon_seen ? "" : " (no beacon heard from this access point)"));
        return;
      }
      _last_try = now_us;
      if (_state == State::Authenticating) send_auth();
      else send_assoc();
    }
    break;
  case State::Handshake:
    if (!_install_at && now_us - _state_since > kHandshakeTimeoutUS)
      fail("no 4-way handshake from the access point");
    break;
  case State::Failed:
    if (now_us - _state_since > (_unsupported.empty() ? kRejoinDelayUS : 60000000ull)) {
      _unsupported.clear();
      std::string error;
      _beacon_seen = false;
      if (!start(_cfg, error)) {
        _state = State::Failed;
        _state_since = now_us;
      }
    }
    break;
  case State::Running:
    /* Negotiate Station TX Block Ack (TID 0 A-MPDU) 1.5s after connecting (once
     * AP authorization and DHCP settle), when DataPlaneAccel (block_ack=1) is on. */
    if (_cfg.block_ack.value_or(false) && _keys_installed && _ap_wmm && (_ap_ht || _ap_vht) &&
        _tx_unicast >= 4 && now_us >= _state_since + 1500000) {
      if (_tx_ba_state == TxBaState::Idle) {
        _tx_ba_state = TxBaState::Asking;
        _tx_ba_tries = 0;
        _tx_ba_last_ask = 0;
      }
      if (_tx_ba_state == TxBaState::Asking && (!_tx_ba_last_ask || now_us - _tx_ba_last_ask > 1000000)) {
        if (++_tx_ba_tries > 3) {
          _tx_ba_state = TxBaState::Failed;
          _log("station: TX Block Ack on TID 0 timed out; sending unaggregated");
        } else {
          _tx_ba_last_ask = now_us;
          std::vector<uint8_t> req(awdl::kAddbaResponseLength);
          awdl::build_addba_request(req.data(), _cfg.bssid, _addr.data(), _cfg.bssid, ++_tx_ba_token, 0, 64,
                                    0, ++_mgmt_seq);
          req[27] |= 0x01; /* A-MSDU supported */
          if (_pmf && _keys_installed) req[1] |= 0x40;
          if (_addba_answers.size() < 8) _addba_answers.push_back(std::move(req));
        }
      }
    }
    flush_tx();
    break;
  default:
    break;
  }
}
