// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Wi-Fi station that runs beside AWDL, host side: join, WPA2-PSK, and the
 * bridge between the access point and Windows' TAP adapter.
 *
 * Supported, all with CCMP in the firmware's key cache: open networks,
 * WPA2-Personal (AKM 2, with management frame protection when the network
 * requires it), WPA2-Personal SHA-256 (AKM 6), WPA3-Personal (SAE, AKM 8,
 * hunting and pecking) and Enhanced Open (OWE, AKM 18). Enterprise (802.1X),
 * SAE hash-to-element only networks, WEP and TKIP are refused at join.
 */
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "iwx_rx_ba.h"
#include "sta_device.h"
#include "sta_tx_policy.h"
#include "tap.h"
#include "wpa3.h"

struct StaConfig {
  std::string ssid;
  uint8_t bssid[6] = {};
  uint8_t channel = 0;
  std::string passphrase; /* 8-63 characters, or empty with `psk_hex` or `open` */
  std::string psk_hex;    /* 64 hex digits */
  bool open = false;
  std::string tap;        /* TAP adapter name or GUID; empty: the first */
  unsigned max_width = 80;
  /* Receive Block Ack sessions with the AP. Unset: the active backend
   * chooses; sta.conf writes 0/1 from the experimental data-plane switch. */
  std::optional<bool> block_ack;
  /* 0: write TAP on the station RX thread. 2: queue and use two writer
   * threads, so the RX thread does not wait for TAP WriteFile. Same switch. */
  unsigned tap_writers = 0;
  /* Experimental Wi-Fi 6: ask an HE access point for HE (802.11ax) when the
   * card can; an AP without HE is joined as before. Off by default. */
  bool he = false;
  /* Optional air MAC address (e.g. Windows' Wi-Fi MAC, #96): used over 802.11
   * and translated at the TAP boundary so factory OUI MACs work without
   * tap-windows6 rejecting non-locally-administered NetworkAddress overrides. */
  bool has_mac = false;
  uint8_t mac[6] = {};

  /* key=value lines: ssid, bssid, channel, passphrase, psk, open, tap, width, ba, tap_writers, he, mac. */
  bool load(const std::string &path, std::string &error);
};

class WifiStation {
public:
  using Logger = std::function<void(const std::string &)>;
  enum class State { Idle, Listening, Authenticating, Associating, Handshake, Running, Failed };

  WifiStation(StaDevice &dev, Logger log) : _dev(dev), _log(std::move(log)) {}

  bool start(const StaConfig &cfg, std::string &error);
  /* Whether a received frame belongs to the station (our BSS). */
  bool owns(const RxFrame &f) const;
  void on_frame(const RxFrame &f);
  /* An Ethernet frame Windows sent on the TAP adapter. */
  void on_ethernet(const uint8_t *frame, size_t len);
  /* Batch TAP TX doorbells across a single inbox drain when TX A-MPDU is active. */
  void begin_tx_batch() { _tx_batching = true; _tx_batch_dirty = false; }
  bool flush_tx();
  bool tx_pending() const {
    return sta_tx::pending(_state == State::Running, _yield, _yield_paused,
                           !_tx_wait.empty(), _dev.sta_tx_agg_queued());
  }
  bool tx_ba_active() const { return _tx_ba_state == TxBaState::Active; }
  /* Timers: call every few milliseconds from the device thread. */
  void tick(uint64_t now_us);
  void stop();

  State state() const { return _state; }
  const char *state_name() const;
  /* Why the network's security cannot be joined ("" when it can or is not
   * known yet): "tkip-group" (WPA/WPA2 mixed: CCMP pairwise, TKIP group),
   * "no-ccmp" (TKIP only) or "wep-or-wpa" (no RSN element: WEP or WPA1). */
  const std::string &unsupported() const { return _unsupported; }
  TapAdapter &tap() { return _tap; }
  uint64_t rx_packets() const { return _rx_packets; }
  uint64_t tx_packets() const { return _tx_packets; }
  /* Bytes of those packets, for the router's traffic class (--coex-plan). */
  uint64_t rx_bytes() const { return _rx_bytes; }
  uint64_t tx_bytes() const { return _tx_bytes; }
  /* Unicast only: Windows' multicast (mDNS, SSDP, LLMNR) never gets answers. */
  uint64_t tx_unicast() const { return _tx_unicast; }
  uint64_t rx_unicast() const { return _rx.unicast; }
  /* Receive Block Ack requests from the AP, answered like AWDL's. */
  bool take_addba(std::vector<uint8_t> &answer);
  /* Data frames from the AP and why some never reached Windows. */
  struct RxStats {
    uint64_t data = 0, unicast = 0, undecrypted = 0, not_llc = 0, amsdu = 0, tap_failed = 0;
    /* Last data frame's firmware rate flags, for the health line: width bits
     * 11-13, VHT/HT MCS in the low bits. */
    uint32_t last_rate = 0;
    uint8_t last_rate_width = 0, last_rate_mcs = 0;
  };
  const RxStats &rx_stats() const { return _rx; }
  /* Off the AP's channel while AWDL carries a transfer (sta_yield.h): a null
   * frame with the power-management bit asks the AP to buffer, then the
   * backend pauses the link; false brings it back and clears the bit.
   * Windows' frames are dropped meanwhile. False if the backend cannot. */
  bool set_yield(bool on);
  bool yielded() const { return _yield; }
  uint64_t yield_dropped() const { return _yield_dropped; }
  /* Leaves and joins again in 5 s (the radio's stall watchdog). */
  void rejoin(const std::string &why) {
    if (_state != State::Idle && _state != State::Failed) fail(why);
  }

private:
  void send_probe();
  void send_auth();
  void send_assoc();
  void handle_mgmt(const RxFrame &f);
  void handle_data(const RxFrame &f);
  void deliver(const uint8_t *da, const uint8_t *sa, const uint8_t *body, size_t len, bool encrypted);
  void handle_eapol(const uint8_t *body, size_t len, bool encrypted);
  bool send_eapol(const std::vector<uint8_t> &eapol, bool protect);
  void reset_join_state();
  /* Key management for this join, from the beacon's RSN element: "" or why not. */
  std::string choose_akm();
  std::vector<uint8_t> rsn_ie() const;
  bool plain() const { return _akm == Akm::None; }
  /* EAPOL-Key descriptor version: 2 (AKM 2), 3 (AKM 6), 0 (AKM-defined: SAE, OWE). */
  uint16_t key_version() const { return _akm == Akm::PskSha256 ? 3 : (_akm == Akm::Sae || _akm == Akm::Owe) ? 0 : 2; }
  bool compute_mic(const uint8_t *key, const uint8_t *data, size_t len, uint8_t out[16]) const;
  const char *akm_name() const;
  void sae_send_commit();
  void sae_send_confirm();
  void handle_sae(uint16_t seq, uint16_t status, const uint8_t *body, size_t len);
  bool install_igtk();
  bool install_gtk();
  void learn_beacon(const uint8_t *f, size_t len);
  void fail(const std::string &why);
  bool send_data(const uint8_t *da, uint16_t ethertype, const uint8_t *payload, size_t len, bool protect);
  void send_null(bool power_save);
  bool derive_ptk(const uint8_t anonce[32], uint8_t out[48]);
  /* The KCK (and KEK at +16) of the handshake in progress: the pending PTK
   * during a rekey, else the one in use. */
  const uint8_t *kck() const { return _rekey ? _tptk : _ptk; }
  /* The GTK KDE from unwrapped key data: a 16-byte CCMP key, else false. */
  bool take_gtk(const std::vector<uint8_t> &plain);
  /* The IGTK KDE (PMF), if the key data carries one. */
  void take_igtk(const std::vector<uint8_t> &plain);
  bool mic_ok(const uint8_t *eapol, size_t len, const uint8_t *key = nullptr);
  void add_mic(std::vector<uint8_t> &eapol, const uint8_t *key = nullptr);

  StaDevice &_dev;
  Logger _log;
  TapAdapter _tap;
  StaConfig _cfg;
  std::array<uint8_t, 6> _addr{};
  std::array<uint8_t, 6> _tap_mac{};
  bool _mac_nat = false;
  State _state = State::Idle;
  uint64_t _now = 0, _state_since = 0, _last_try = 0;
  unsigned _tries = 0, _joins = 0;
  /* set_yield: the null frame goes first, the link pauses at _yield_at. */
  bool _yield = false, _yield_paused = false, _yield_unsupported = false;
  uint64_t _yield_at = 0, _yield_dropped = 0;

  /* Key management: AKM suite type in 00-0F-AC (0 = none, an open network). */
  enum class Akm : uint8_t { None = 0, Psk = 2, PskSha256 = 6, Sae = 8, Owe = 18 };
  Akm _akm = Akm::Psk;
  bool _pmf = false; /* management frame protection negotiated */

  /* What the beacon told us. */
  bool _beacon_seen = false, _ap_rsn = false, _ap_ht = false, _ap_vht = false, _ap_2ss = false, _ap_wmm = false;
  bool _ap_pairwise_ccmp = true, _ap_mfpc = false, _ap_mfpr = false, _ap_bip_cmac = true, _ap_h2e_only = false;
  bool _ap_rsn_any = false;      /* an RSN element at all, whatever its ciphers */
  uint8_t _ap_group_cipher = 0;  /* RSN group cipher suite type (2 TKIP, 4 CCMP), 0 unknown */
  std::string _unsupported;
  uint32_t _ap_akms = 0; /* bit n: AKM 00-0F-AC:n offered */
  uint16_t _ap_rsn_caps = 0;
  std::vector<uint8_t> _ap_rsn_raw;

  /* SAE (WPA3-Personal) */
  enum class SaeStep : uint8_t { None, Committed, Confirmed };
  wpa3::Sae _sae;
  SaeStep _sae_step = SaeStep::None;
  std::vector<uint8_t> _sae_token;
  uint16_t _sae_sc = 0;
  unsigned _sae_tokens = 0;
  /* OWE */
  wpa3::Owe _owe;
  /* PMF: the integrity group key */
  uint8_t _igtk[16] = {}, _igtk_in_fw[16] = {};
  unsigned _igtk_id = 4;
  uint64_t _igtk_ipn = 0;
  bool _igtk_valid = false, _igtk_in_fw_valid = false;
  uint16_t _ap_capab = 0, _beacon_int = 100;
  uint8_t _dtim = 1;
  unsigned _ap_width = 20;
  uint16_t _ap_ht_cap = 0, _ap_vht_mcs = 0xffff;
  uint8_t _ap_ampdu = 0;
  uint32_t _ap_vht_cap = 0;
  sta_he::ApHe _ap_he;   /* from the beacon */
  bool _he_asked = false; /* the last association request carried HE Capabilities */
  std::vector<uint8_t> _ap_rates;

  /* WPA2 */
  uint8_t _pmk[32] = {};
  uint8_t _ptk[48] = {}; /* KCK 16 | KEK 16 | TK 16 */
  uint8_t _tptk[48] = {}; /* a rekey's PTK until it is installed */
  bool _rekey = false, _ptk_installed = false;
  uint64_t _rekey_since = 0;
  uint8_t _gtk_in_fw[16] = {}, _gtk_in_fw_idx = 0;
  bool _gtk_in_fw_valid = false;
  uint8_t _ba_token[8] = {}, _ba_token_valid = 0;
  uint16_t _ba_ssn[8] = {}, _ba_window[8] = {};
  bool _m4_unsent = false; /* 4/4 could not be queued; its keys wait for the resend */
  /* The last EAPOL reply, sent again when the AP repeats its message. */
  std::vector<uint8_t> _last_reply;
  bool _last_reply_protect = false;
  uint8_t _last_reply_replay[8] = {};
  unsigned _declined_ba = 0;
  uint8_t _snonce[32] = {}, _anonce[32] = {};
  uint8_t _replay[8] = {};
  bool _replay_valid = false, _keys_installed = false, _ptk_ready = false;
  uint64_t _install_at = 0;
  uint8_t _gtk[32] = {};
  unsigned _gtk_len = 0, _gtk_index = 1;
  std::vector<uint8_t> _rsn_ie;

  std::deque<std::vector<uint8_t>> _addba_answers;
  /* Station TX Block Ack (TID 0, A-MPDU) gated by _cfg.block_ack. */
  enum class TxBaState : uint8_t { Idle, Asking, Active, Failed };
  TxBaState _tx_ba_state = TxBaState::Idle;
  uint8_t _tx_ba_token = 0;
  unsigned _tx_ba_tries = 0;
  uint64_t _tx_ba_last_ask = 0;
  bool _tx_batching = false, _tx_batch_dirty = false;
  struct PendingEth { std::vector<uint8_t> frame; bool protect = false; };
  std::deque<PendingEth> _tx_wait;
  bool transmit_qos_agg(const uint8_t *da, uint16_t ethertype, const uint8_t *payload, size_t len, bool protect, bool kick);
  uint16_t _mgmt_seq = 0, _qos_seq = 0;
  iwx::RxBaReorder _rx_reorder;
  uint64_t _rx_packets = 0, _tx_packets = 0, _tx_unicast = 0, _rx_bytes = 0, _tx_bytes = 0;
  RxStats _rx;
};
