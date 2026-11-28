// SPDX-License-Identifier: GPL-2.0-only
/* Checks that need no card: the firmware ABI struct sizes the driver depends
 * on, that each bundled Qu/QuZ firmware parses into the section layout
 * and command versions the bring-up expects, and the radio backend contract
 * (tests/radio_backend_test.h).
 * Usage: iwx_selftest <firmware dir> */
#include <cstdio>
#include <string>

#include "../src/iwxreg.h"
#include "../src/iwx_channel.h"
#include "../src/dma_runs.h"
#include "../src/cards.h"
#include "../src/fw.h"
#include "../src/awdl_policy.h"
#include "../src/coex_plan.h"
#include "../src/sta_yield.h"
#include "../src/crypto.h"
#include "../src/iwx_rx_ba.h"
#include "../src/wpa3.h"
#include "../src/sta_he.h"
#include "../src/sta_mac_nat.h"
#include "../src/sta_tx_policy.h"
#include "radio_backend_test.h"
#include <cstring>
#include <vector>

static int failures = 0;

#define EXPECT_SIZE(type, n)                                                            \
  do {                                                                                  \
    if (sizeof(type) != (n)) {                                                          \
      std::printf("FAIL sizeof(%s) = %zu, expected %d\n", #type, sizeof(type), (n));    \
      failures++;                                                                       \
    } else {                                                                            \
      std::printf("ok   sizeof(%s) = %d\n", #type, (n));                                \
    }                                                                                   \
  } while (0)

int main(int argc, char **argv) {
  static_assert(iwx_primary80_position(140) == IWX_PHY_VHT_CTRL_POS_1_ABOVE);
  static_assert(iwx_primary80_position(136) == IWX_PHY_VHT_CTRL_POS_1_BELOW);
  static_assert(iwx_primary80_position(144) == IWX_PHY_VHT_CTRL_POS_2_ABOVE);
  static_assert(iwx_primary80_position(149) == IWX_PHY_VHT_CTRL_POS_2_BELOW);
  static_assert(iwx_primary80_position(14) == -1 && iwx_primary80_position(146) == -1);
  {
    /* An early 4-page ring must not consume the only 8-page firmware span,
     * and RX's individual pages cannot take either reservation. */
    DmaRuns runs;
    runs.reset({100,101,102,103,104,105,106,107, 200,201,202,203, 300});
    const bool impossible = runs.reserve({9}).has_value();
    auto planned = runs.reserve({4, 8});
    if (impossible || !planned || planned->count(4) != 1 || planned->count(8) != 1 ||
        planned->find(4)->second != 8 || planned->find(8)->second != 0 ||
        runs.take(1) != std::optional<size_t>{12} || runs.take(1)) {
      std::printf("FAIL fragmented DMA runs (#30)\n");
      failures++;
    } else {
      std::printf("ok   fragmented DMA runs preserve firmware and rings (#30)\n");
    }
  }
  /* Sizes from Linux iwlwifi's fw/api headers for the same API versions. */
  EXPECT_SIZE(iwx_tfh_tb, 10);
  EXPECT_SIZE(iwx_tfh_tfd, 256);
  EXPECT_SIZE(iwx_cmd_header, 4);
  EXPECT_SIZE(iwx_cmd_header_wide, 8);
  EXPECT_SIZE(iwx_device_cmd, 324);
  EXPECT_SIZE(iwx_tx_cmd_gen2, 20);
  EXPECT_SIZE(iwx_rb_status, 12);
  EXPECT_SIZE(iwx_tx_queue_cfg_cmd, 24);
  EXPECT_SIZE(iwx_tx_queue_cfg_rsp, 8);
  EXPECT_SIZE(iwx_binding_cmd, 28);
  EXPECT_SIZE(iwx_phy_context_cmd, 32);
  EXPECT_SIZE(iwx_add_sta_cmd, 48); /* ADD_STA_CMD_API_S_VER_10 */
  EXPECT_SIZE(iwx_mcc_update_cmd, 28);
  EXPECT_SIZE(iwx_alive_resp_v4, 0x74);
  EXPECT_SIZE(iwx_tx_resp, 44);
  /* AX210 family */
  EXPECT_SIZE(iwx_tx_cmd_gen3, 28);          /* TX_CMD_API_S_VER_8 */
  EXPECT_SIZE(iwx_rx_transfer_desc, 16);
  EXPECT_SIZE(iwx_rx_completion_desc, 32);
  EXPECT_SIZE(iwx_gen3_bc_tbl, 2048);
  EXPECT_SIZE(iwx_context_info_gen3, 104);   /* IPC_CONTEXT_INFO_S */
  EXPECT_SIZE(iwx_alive_resp_v5, 0x74 + 12); /* v4 + sku_id */
  /* Bz family (BE200) and MLD API */
  EXPECT_SIZE(iwx_rx_completion_desc_bz, 4);
  EXPECT_SIZE(iwx_bz_bc_tbl, 8192);
  EXPECT_SIZE(iwx_prph_scratch_mem_desc_addr_array, 256);
  EXPECT_SIZE(iwx_mac_config_cmd, 52);       /* MAC_CONTEXT_CONFIG_CMD_API_S_VER_2 / _VER_3 */
  EXPECT_SIZE(iwx_link_config_cmd, 208);     /* LINK_CONTEXT_CONFIG_CMD_API_S_VER_1..9 */
  EXPECT_SIZE(iwx_sta_cfg_cmd_v1, 96);       /* STA_CMD_API_S_VER_1 */
  EXPECT_SIZE(iwx_sta_cfg_cmd_v2, 104);      /* STA_CMD_API_S_VER_2 */
  EXPECT_SIZE(iwx_remove_sta_cmd, 4);        /* REMOVE_STA_API_S_VER_1 */
  EXPECT_SIZE(iwx_rlc_config_cmd, 32);       /* RLC_CONFIG_CMD_API_S_VER_2 */
  EXPECT_SIZE(iwx_mcc_update_resp_v8, 24);   /* LAR_UPDATE_MCC_CMD_RESP_S_VER_8 */
  EXPECT_SIZE(iwx_rx_baid_cfg_cmd, 16);      /* RX_BAID_ALLOCATION_CONFIG_CMD_API_S_VER_2 */
  EXPECT_SIZE(iwx_rx_baid_cfg_resp, 4);      /* RX_BAID_ALLOCATION_RESPONSE_API_S_VER_1 */
  EXPECT_SIZE(iwx_scd_queue_cfg_cmd, 36);    /* TX_QUEUE_CFG_CMD_API_S_VER_3 */
  EXPECT_SIZE(iwx_sec_key_cmd, 80);          /* SEC_KEY_CMD_API_S_VER_1 */
  EXPECT_SIZE(iwx_tlc_config_cmd_v4, 28);    /* TLC_MNG_CONFIG_CMD_API_S_VER_4 */
  EXPECT_SIZE(iwx_scan_config, 12);          /* SCAN_CONFIG_DB_CMD_API_S_5 */

  /* Card selection (cards.h), with register values as iwx keeps them. */
  struct CardCase { uint16_t dev; uint32_t hw_rev, rf_id; const char *fw; bool ax210, bz, integrated; };
  const CardCase cases[] = {
      {0x43F0, 0x354, 0x0010a100, "iwlwifi-QuZ-a0-hr-b0-68.ucode", false, false, true}, /* this project's AX201 */
      {0xA0F0, 0x334, 0x0010a100, "iwlwifi-Qu-b0-hr-b0-68.ucode", false, false, true},
      {0x34F0, 0x338, 0x0010a100, "iwlwifi-Qu-c0-hr-b0-68.ucode", false, false, true},
      {0x02F0, 0x354, 0x00108000, "iwlwifi-QuZ-a0-jf-b0-68.ucode", false, false, true},  /* 9560 on QuZ */
      {0x2723, 0x340, 0x0010a100, "iwlwifi-cc-a0-68.ucode", false, false, false},        /* AX200 */
      {0x2725, 0x420, 0x0010d000, "iwlwifi-ty-a0-gf-a0-68.ucode", true, false, false},   /* AX210: discrete */
      {0x7AF0, 0x370, 0x0010d000, "iwlwifi-so-a0-gf-a0-68.ucode", true, false, true},    /* AX211 */
      {0x51F0, 0x370, 0x1010d000, "iwlwifi-so-a0-gf4-a0-68.ucode", true, false, true},   /* AX411 */
      {0x272B, 0x470, 0x00112000, "iwlwifi-gl-c0-fm-c0-100.ucode", true, true, false},   /* BE200: discrete Gl/Fm */
      {0x272B, 0x470, 0x0010d000, nullptr, true, true, false},                           /* Gl with non-FM radio */
      {0x7740, 0x460, 0x0010a000, "iwlwifi-bz-b0-hr-b0-100.ucode", true, true, true},     /* AX203 on Arrow Lake */
      {0x2726, 0x420, 0x0010d000, nullptr, true, false, true},                           /* SnJ: no firmware */
      {0x1234, 0x354, 0x0010a100, nullptr, false, false, true},
  };
  for (const auto &k : cases) {
    const CardInfo c = cards::select(k.dev, k.hw_rev, k.rf_id);
    const bool ok = k.fw ? (c.known && c.firmware == k.fw && c.ax210 == k.ax210 && c.bz == k.bz &&
                            c.integrated == k.integrated)
                         : !c.known;
    std::printf("%s card %04x rev %03x rf %08x -> %s%s\n", ok ? "ok  " : "FAIL", k.dev, k.hw_rev, k.rf_id,
                c.known ? c.firmware.c_str() : "unsupported: ", c.known ? "" : c.why.c_str());
    if (!ok) failures++;
  }
  {
    const CardInfo c = cards::select(0x272B, 0x470, 0x00112000);
    if (!c.known || !c.ax210 || !c.bz || c.integrated || c.xtal_latency != 12000 ||
        !c.low_latency_xtal || c.tested || cards::family_device("be200") != 0x272B) {
      std::printf("FAIL BE200 transport parameters or family_device(\"be200\")\n");
      failures++;
    }
  }
  {
    const CardInfo c = cards::select(0x7740, 0x460, 0x0010a000);
    if (!c.known || !c.ax210 || !c.bz || !c.integrated || c.xtal_latency != 12000 ||
        c.ltr_delay != IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500 || !c.low_latency_xtal || !c.tested ||
        c.pnvm != "iwlwifi-bz-b0-hr-b0.pnvm")
      std::printf("FAIL AX203 (8086:7740) transport parameters, PNVM or tested flag\n"), failures++;
  }
  {
    const CardInfo c = cards::select(0x43F0, 0x354, 0x0010a100);
    if (c.xtal_latency != 12000 || c.ltr_delay != IWX_SOC_FLAGS_LTR_APPLY_DELAY_2500 || !c.low_latency_xtal || !c.tested) {
      std::printf("FAIL the tested AX201 lost its transport parameters\n");
      failures++;
    }
  }
  {
    /* The Raptor Lake AX211 (issue #2): Linux's so_long_latency_imr parameters. */
    const CardInfo c = cards::select(0x7A70, 0x370, 0x0010d000);
    if (!c.known || !c.integrated || !c.imr || !c.low_latency_xtal || c.xtal_latency != 12000) {
      std::printf("FAIL the 7A70 AX211 lost its IMR / long-latency parameters\n");
      failures++;
    }
  }
  std::printf("info IWX_RX_DESC_SIZE_V1 = %zu, sizeof(iwx_context_info) = %zu, "
              "sizeof(iwx_mac_ctx_cmd) = %zu\n",
              (size_t)IWX_RX_DESC_SIZE_V1, sizeof(iwx_context_info), sizeof(iwx_mac_ctx_cmd));

  if (argc > 1) {
    const char *names[] = {"iwlwifi-Qu-b0-hr-b0-68.ucode", "iwlwifi-Qu-c0-hr-b0-68.ucode",
                           "iwlwifi-QuZ-a0-hr-b0-68.ucode", "iwlwifi-Qu-b0-jf-b0-68.ucode",
                           "iwlwifi-Qu-c0-jf-b0-68.ucode", "iwlwifi-QuZ-a0-jf-b0-68.ucode",
                           "iwlwifi-cc-a0-68.ucode", "iwlwifi-so-a0-gf-a0-68.ucode",
                           "iwlwifi-so-a0-gf4-a0-68.ucode", "iwlwifi-so-a0-hr-b0-68.ucode",
                           "iwlwifi-so-a0-jf-b0-68.ucode", "iwlwifi-ty-a0-gf-a0-68.ucode"};
    for (const char *name : names) {
      Firmware fw;
      std::string error;
      const std::string path = std::string(argv[1]) + "\\" + name;
      if (!fw.load(path, error)) {
        std::printf("FAIL %s: %s\n", name, error.c_str());
        failures++;
        continue;
      }
      size_t lmac = 0, umac = 0, paging = 0, seps = 0;
      for (const auto &s : fw.regular) {
        if (s.devoff == IWX_CPU1_CPU2_SEPARATOR_SECTION || s.devoff == IWX_PAGING_SEPARATOR_SECTION)
          seps++;
        else if (seps == 0) lmac++;
        else if (seps == 1) umac++;
        else paging++;
      }
      size_t largest = 0;
      for (const auto &s : fw.regular) largest = s.len > largest ? s.len : largest;
      std::printf("ok   %s: version %s, %zu LMAC / %zu UMAC / %zu paging sections, largest %zu "
                  "bytes, phy_config 0x%x\n",
                  name, fw.version.c_str(), lmac, umac, paging, largest, fw.phy_config);
      std::printf("     cmd versions: TX_CMD %u, PHY_CONTEXT %u, ADD_STA %u, SCAN_REQ_UMAC %u, "
                  "MAC_CONTEXT %u; DQA %d, LAR %d, CDB %d, UHB %d, NVM_INFO_v4 %d\n",
                  fw.cmd_version(IWX_LONG_GROUP, IWX_TX_CMD),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_PHY_CONTEXT_CMD),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_ADD_STA),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_SCAN_REQ_UMAC),
                  fw.cmd_version(IWX_LONG_GROUP, IWX_MAC_CONTEXT_CMD),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_DQA_SUPPORT), fw.has_capa(IWX_UCODE_TLV_CAPA_LAR_SUPPORT),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_CDB_SUPPORT),
                  fw.has_capa(IWX_UCODE_TLV_CAPA_ULTRA_HB_CHANNELS),
                  fw.has_api(IWX_UCODE_TLV_API_REGULATORY_NVM_INFO));
      const bool ax210 = std::strncmp(name, "iwlwifi-so-", 11) == 0 || std::strncmp(name, "iwlwifi-ty-", 11) == 0;
      if (ax210 && !fw.iml_len) {
        std::printf("FAIL %s: an AX210-family firmware without an image loader\n", name);
        failures++;
      }
      if (largest > (2u << 20)) {
        std::printf("FAIL a section is larger than one 2 MiB DMA block\n");
        failures++;
      }
    }
    Firmware be200;
    std::string be200_error;
    if (!be200.load(std::string(argv[1]) + "\\iwlwifi-gl-c0-fm-c0-100.ucode", be200_error)) {
      std::printf("FAIL BE200 firmware: %s\n", be200_error.c_str());
      failures++;
    } else if (!be200.has_capa(IWX_UCODE_TLV_CAPA_MLD_API_SUPPORT) ||
               !be200.has_api(IWX_UCODE_TLV_API_SMART_FIFO_OFFLOAD)) {
      std::printf("FAIL BE200 firmware did not advertise MLD and Smart FIFO offload\n");
      failures++;
    } else {
      std::printf("ok   BE200 firmware %s advertises MLD and Smart FIFO offload\n", be200.version.c_str());
    }
  }

  /* AWDL policy (ported from ForbiddenItlwm), on a captured iPhone frame. */
  {
#define CHECK(cond, what)                                                                   \
  do {                                                                                      \
    if (cond) std::printf("ok   %s\n", what);                                               \
    else {                                                                                  \
      std::printf("FAIL %s\n", what);                                                       \
      failures++;                                                                           \
    }                                                                                       \
  } while (0)
    /* TLV 18 as an iPhone in Japan sent it on 2026-11-16, and TLV 12 "JP". */
    const uint8_t tlv18[] = {0x0f, 0x03, 0x00, 0x03, 0xff, 0xff, 0x2c, 0x80, 0x2c, 0x80, 0x2c, 0x80,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x80, 0x06, 0x51,
                             0x2c, 0x80, 0x2c, 0x80, 0x2c, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                             0x2c, 0x80, 0x00, 0x00, 0x00};
    std::vector<uint8_t> f(40, 0);
    f[0] = 0xd0; f[24] = 0x7f; f[25] = 0x00; f[26] = 0x17; f[27] = 0xf2; f[28] = 0x08;
    f.push_back(18); f.push_back(sizeof(tlv18)); f.push_back(0);
    f.insert(f.end(), tlv18, tlv18 + sizeof(tlv18));
    const uint8_t tlv12[] = {12, 4, 0, 0, 0, 'J', 'P'};
    f.insert(f.end(), tlv12, tlv12 + sizeof(tlv12));
    CHECK(awdl::is_awdl_action(f.data(), f.size()), "AWDL action frame recognised");
    {
      /* Station pause during AWDL transfers (sta_yield.h), 100 ms ticks. */
      using sta_yield::Action;
      sta_yield::Controller y;
      uint64_t t = 1000000, n = 0;
      Action a = Action::None;
      for (int i = 0; i < 30; i++, t += 100000) a = y.tick(t, n += (i % 3 == 0), true);
      CHECK(!y.paused, "station stays on the air for AWDL discovery's trickle (3 frames/s)");
      for (int i = 0; i < 10 && !y.paused; i++, t += 100000) a = y.tick(t, n += 5, true);
      CHECK(y.paused && a == Action::Pause && y.why == sta_yield::Why::Busy, "an AWDL transfer pauses the station");
      sta_yield::Controller z = y;
      CHECK(z.tick(t, n + 50, false) == Action::Resume, "station down or on AWDL's channel ends the pause");
      for (int i = 0; i < 5; i++, t += 100000) a = y.tick(t, n += 5, true);
      CHECK(y.paused, "pause holds while the transfer runs");
      bool resumed = false;
      for (int i = 0; i < 30; i++, t += 100000) a = y.tick(t, i == 0 ? n += 40 : n, true);
      CHECK(y.paused, "a 3 s retransmission gap does not end the pause");
      for (int i = 0; i < 80 && !resumed; i++, t += 100000) resumed = y.tick(t, n, true) == Action::Resume;
      CHECK(resumed && y.why == sta_yield::Why::Idle, "AWDL idle for 5 s brings the station back");
      for (int i = 0; i < 300 && y.why != sta_yield::Why::Visit; i++, t += 100000) y.tick(t, n += 5, true);
      CHECK(y.why == sta_yield::Why::Visit && !y.paused, "a long transfer still visits the access point");
      bool early = false;
      const uint64_t back = t;
      for (; t - back < sta_yield::kVisitUS - 100000; t += 100000) early |= y.tick(t, n += 5, true) == Action::Pause;
      CHECK(!early, "the visit lasts its full time");
    }
    {
      awdl::AddbaRequest r{};
      r.token = 9;
      r.tid = 0;
      r.window = 64;
      uint8_t own[6] = {2, 0, 0, 0, 0, 1}, out[awdl::kAddbaResponseLength];
      awdl::build_addba_response(out, r, own, 1, false, awdl::kAddbaDeclined);
      CHECK(out[26] == 9 && out[27] == 37 && out[28] == 0, "ADDBA declined: token kept, status 37");
      awdl::build_addba_response(out, r, own, 1);
      CHECK(out[27] == 0 && out[28] == 0, "ADDBA accepted: status 0");
    }
    {
      /* FCS (#24/#25): CRC-32 check value, then a frame with its FCS. */
      const uint8_t nine[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
      CHECK(awdl::crc32_ieee(nine, 9) == 0xCBF43926u, "CRC-32 check value");
      CHECK(awdl::crc32_ieee(nullptr, 0) == 0, "empty CRC-32");
      /* Independent IEEE CRC-32 vectors for unaligned input and every tail length. */
      alignas(8) uint8_t pattern[72];
      for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = static_cast<uint8_t>(i * 37 + 11);
      constexpr uint32_t expected[] = {0xA454924Bu, 0x63AD523Bu, 0xE30FCC30u, 0xDF8C8A7Fu,
                                       0x040BC0EFu, 0xE5DEDFBDu, 0x2585660Fu, 0xD3FC1BEDu};
      bool tails_ok = true;
      for (size_t tail = 0; tail < 8; ++tail)
        tails_ok &= awdl::crc32_ieee(pattern + 1, 64 + tail) == expected[tail];
      CHECK(tails_ok, "CRC-32 with unaligned monitor frames and partial final words");
      std::vector<uint8_t> g = f;
      g.push_back(21); g.push_back(2); g.push_back(0); g.push_back(3); g.push_back(2); /* version TLV */
      const size_t bare = g.size();
      CHECK(awdl::strip_valid_fcs(g.data(), bare) == bare, "frame without FCS kept whole");
      const awdl::AwdlTlvFit fit = awdl::awdl_tlv_fit(g.data(), bare);
      CHECK(fit.left == 0 && fit.has_version, "TLVs fill the frame, version TLV found");
      const uint32_t fcs = awdl::crc32_ieee(g.data(), bare);
      for (int i = 0; i < 4; i++) g.push_back(static_cast<uint8_t>(fcs >> (8 * i)));
      CHECK(awdl::strip_valid_fcs(g.data(), g.size()) == bare, "valid FCS removed");
      CHECK(awdl::awdl_tlv_fit(g.data(), g.size()).left == 4, "FCS left on shows as 4 bytes after the TLVs");
      g.back() ^= 1;
      CHECK(awdl::strip_valid_fcs(g.data(), g.size()) == g.size(), "wrong FCS kept");
    }
    {
      /* #4: our DELBA as the session's sender, and when an idle session ends. */
      const uint8_t phone[6] = {2, 1, 1, 1, 1, 1}, pc[6] = {2, 2, 2, 2, 2, 2};
      const uint8_t bss[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
      uint8_t d[awdl::kDelbaLength];
      CHECK(awdl::build_delba(d, phone, pc, bss, 0, true, 7) == 30, "DELBA is 30 bytes");
      bool sender = false;
      uint8_t tid = 9;
      CHECK(awdl::parse_delba(d, sizeof(d), phone, &sender, &tid) && sender && tid == 0,
            "DELBA reads back as the sender's, TID 0");
      CHECK(!awdl::ba_tx_session_idle(true, 0, 0, 1000000, 500000, 9000000), "BA session kept during a bulk send");
      CHECK(!awdl::ba_tx_session_idle(false, 3, 0, 1000000, 500000, 9000000), "BA session kept while frames wait");
      CHECK(!awdl::ba_tx_session_idle(false, 0, 1, 1000000, 500000, 9000000), "BA session kept while frames are queued");
      CHECK(!awdl::ba_tx_session_idle(false, 0, 0, 1000000, 500000, 2000000), "BA session kept 1 s after the last A-MPDU");
      CHECK(!awdl::ba_tx_session_idle(false, 0, 0, 1000000, 500000, 3400000), "BA session kept 2.4 s after the last A-MPDU");
      CHECK(awdl::ba_tx_session_idle(false, 0, 0, 1000000, 500000, 3600000), "BA session ended 2.6 s after the last A-MPDU");
      CHECK(awdl::ba_tx_session_idle(false, 0, 0, 0, 500000, 3100000), "BA session with no A-MPDU ends 2.6 s after agreeing");
    }
    {
      /* --coex-plan (ForbiddenItlwm PR #30). */
      using namespace awdl_plan;
      uint8_t away[16];
      CHECK(!plan_entries(true, 44, 80, 44, false, InfraClass::Idle, away), "plan: station on the AWDL channel, no router entries");
      CHECK(!plan_entries(true, 6, 20, 149, false, InfraClass::Idle, away), "plan: 2.4 GHz station keeps none");
      CHECK(!plan_entries(false, 40, 80, 149, false, InfraClass::Idle, away), "plan: no station, none");
      CHECK(plan_entries(true, 44, 160, 44, false, InfraClass::Idle, away), "plan: 160 MHz station over the AWDL channel");
      CHECK(plan_entries(true, 40, 80, 149, false, InfraClass::Idle, away) && away[6] && away[7] && away[14] &&
                away[15] && !away[0] && !away[13], "plan: AWDL idle: entries 6, 7, 14, 15");
      CHECK(plan_entries(true, 40, 80, 149, true, InfraClass::Idle, away) && away[15] && !away[6],
            "plan: AWDL data, router idle: entry 15 only");
      CHECK(plan_entries(true, 100, 80, 149, true, InfraClass::Data, away) && away[4] && away[12] && !away[11],
            "plan: router on a radar channel allowed; router data: 4, 5, 12-15");
      CHECK(plan_entries(true, 40, 80, 149, true, InfraClass::Streaming, away) && away[11],
            "plan: router streaming adds entry 11");
      InfraMeter m;
      m.note(1000000, 0, 0);
      CHECK(m.note(2000000, 200, 200 * 300) && m.cls == InfraClass::Data, "plan: 200 small packets/s is data");
      CHECK(m.note(3000000, 260, 200 * 300 + 60 * 1500) && m.cls == InfraClass::Streaming, "plan: big packets are streaming");
      CHECK(!m.note(3500000, 999, 0), "plan: sampled once a second");
      /* A frame of ours: TLV 4 (33 bytes, AW counter 0x0123 at 29, then a
       * 16-entry op-class sequence) and TLV 18 (the same sequence). */
      std::vector<uint8_t> g(40, 0);
      g[0] = 0xd0; g[24] = 0x7f; g[25] = 0x00; g[26] = 0x17; g[27] = 0xf2; g[28] = 0x08;
      std::vector<uint8_t> cs = {15, 3, 0, 3, 0xff, 0xff};
      for (int k = 0; k < 16; k++) { cs.push_back(149); cs.push_back(124); }
      std::vector<uint8_t> t4(33, 0);
      t4[29] = 0x23; t4[30] = 0x01;
      t4.insert(t4.end(), cs.begin(), cs.end());
      t4.push_back(0); t4.push_back(0);
      g.push_back(4); g.push_back(uint8_t(t4.size())); g.push_back(0);
      g.insert(g.end(), t4.begin(), t4.end());
      const size_t t18 = g.size();
      g.push_back(18); g.push_back(uint8_t(cs.size() + 3)); g.push_back(0);
      g.insert(g.end(), cs.begin(), cs.end());
      g.push_back(0); g.push_back(0); g.push_back(0);
      CHECK(frame_aw_counter(g.data(), g.size()) == 0x0123, "plan: AW counter read from TLV 4");
      CHECK(in_step(0x0123, 0x0123 / 4) && in_step(0x0123, 0x0123 / 4 + 1) && !in_step(0x0123, 0x0123 / 4 + 3),
            "plan: in step within one extended AW");
      plan_entries(true, 40, 80, 149, false, InfraClass::Idle, away);
      CHECK(rewrite_frame(g.data(), g.size(), away, 40), "plan: frame rewritten");
      const uint8_t *e18 = g.data() + t18 + 3 + 6;
      CHECK(e18[2 * 6] == 40 && e18[2 * 6 + 1] == 115 && e18[2 * 15] == 40 && e18[0] == 149 && e18[2 * 13] == 149,
            "plan: TLV 18 names channel 40 (class 115) in 6/15, 149 elsewhere");
      const uint8_t *e4 = g.data() + 40 + 3 + 33 + 6;
      CHECK(e4[2 * 7] == 40 && e4[2 * 8] == 149, "plan: TLV 4's sequence rewritten the same way");
      CHECK(awdl::awdl_tlv_fit(g.data(), g.size()).left == 0, "plan: rewritten frame keeps its TLV layout");
    }
    uint8_t seq[16];
    CHECK(awdl::peer_sequence(f.data(), f.size(), seq), "channel sequence parsed");
    CHECK(awdl::peer_channel(seq) == 44, "iPhone in Japan: peer 5 GHz channel is 44");
    char c[3];
    CHECK(awdl::peer_country(f.data(), f.size(), c) && std::string(c) == "JP", "peer country JP");
    awdl::PeerCountry pc;
    bool changed = false;
    for (int k = 0; k < 5; k++) changed |= pc.note(c);
    CHECK(changed && std::string(pc.confirmed) == "JP", "country confirmed after 5 frames");
    uint8_t s149[16] = {149, 149, 149, 0, 0, 0, 0, 0, 6, 149, 149, 149, 0, 0, 0, 0};
    CHECK(awdl::peer_channel(s149) == 149, "sequence naming 149 gives 149");
    uint8_t wifi44[16] = {149, 149, 0, 0, 0, 0, 44, 44, 149, 149, 0, 0, 0, 0, 44, 44};
    CHECK(awdl::peer_channel(wifi44) == 149, "44 only in Wi-Fi slots, 149 named: 149");

    /* ADDBA request -> response round trip. */
    const uint8_t own[6] = {0x02, 1, 2, 3, 4, 5}, peer[6] = {0x66, 0x71, 0xa5, 0xbe, 0xeb, 0xb3};
    uint8_t req[33] = {};
    req[0] = 0xd0;
    std::memcpy(req + 4, own, 6);
    std::memcpy(req + 10, peer, 6);
    req[24] = 3; req[25] = 0; req[26] = 7;
    const uint16_t params = uint16_t(2 | (5 << 2) | (64 << 6));
    req[27] = uint8_t(params); req[28] = uint8_t(params >> 8);
    req[31] = uint8_t(123 << 4); req[32] = uint8_t(123 >> 4);
    awdl::AddbaRequest r;
    CHECK(awdl::parse_addba_request(req, sizeof(req), own, &r) && r.tid == 5 && r.window == 64 &&
              r.ssn == 123 && r.token == 7 && r.immediate,
          "ADDBA request parsed (tid 5, window 64, ssn 123)");
    uint8_t ans[33];
    awdl::build_addba_response(ans, r, own, 9);
    CHECK(ans[25] == 1 && ans[26] == 7 && std::memcmp(ans + 4, peer, 6) == 0 &&
              (((ans[29] | ans[30] << 8) >> 2) & 15) == 5,
          "ADDBA response built for the same token and tid");

    awdl::BaFilter bf;
    bf.note(1000000, 0);
    CHECK(bf.note(2000001, 40) && bf.off, "busy data turns promiscuous off");
    bf.note(3000002, 41); bf.note(4000003, 42);
    CHECK(bf.note(5000004, 43) && !bf.off, "three quiet seconds turn it back on");

    awdl::RateLadder rl;
    for (int k = 0; k < 16; k++) rl.note(true, false);
    CHECK(rl.step == 1, "16 clean frames step the rate up");
    for (int k = 0; k < 16; k++) rl.note(k % 3 != 0, false);
    CHECK(rl.step == 0, "a third lost steps it back down");
    for (int k = 0; k < 16 * 8; k++) rl.note(true, false);
    CHECK(rl.step == 3, "20 MHz tops out at HT MCS 7");
    for (int k = 0; k < 16 * 8; k++) rl.note(true, true);
    CHECK(rl.step == 6, "80 MHz reaches VHT 2x2 MCS 8");
    for (int k = 0; k < 4; k++) rl.note(false, true);
    CHECK(rl.step == 5, "4 consecutive failures step down immediately (#91)");
    rl.note_give_up();
    CHECK(rl.step == 3, "abandoned unicast frame steps down by 2 (#91)");
    CHECK(awdl::rssi_initial_rate_step(-42, true, 1) == 5, "strong 80 MHz RSSI (>=-46 dBm) seeds VHT80 MCS 7");
    CHECK(awdl::rssi_initial_rate_step(-52, true, 1) == 4, "good 80 MHz RSSI (>=-54 dBm) seeds VHT80 MCS 4");
    CHECK(awdl::rssi_initial_rate_step(-58, true, 1) == 1, "moderate/weak RSSI keeps conservative base step");
    CHECK(awdl::rssi_initial_rate_step(-40, false, 1) == 1, "20/40 MHz keeps conservative base step");
    CHECK(awdl::retry_rate_step(6, 0) == 6 && awdl::retry_rate_step(6, 1) == 4 && awdl::retry_rate_step(6, 2) == 0,
          "retries fall back two steps then Legacy 24 (#91)");

    awdl::BaTxBulk bulk;
    bulk.note(1000000, 0, false);
    CHECK(!bulk.note(1005000, 5, false) && !bulk.bulk, "BaTxBulk waits for 6 payload frames");
    CHECK(bulk.note(1006000, 6, false) && bulk.bulk, "BaTxBulk triggers immediately at 6+ payload frames");
    for (int k = 1; k <= 3; k++) CHECK(!bulk.note(1006000 + uint64_t(k) * 250000, 6, false) && bulk.bulk, "BaTxBulk holds across brief quiet");
    CHECK(bulk.note(2006000, 6, false) && !bulk.bulk, "BaTxBulk exits after 4 quiet windows (1 s)");
  }

  /* WPA2 crypto against published vectors. */
  {
    const auto hex_eq = [](const uint8_t *p, const char *hex) {
      for (size_t i = 0; hex[2 * i]; i++) {
        unsigned v;
        std::sscanf(hex + 2 * i, "%2x", &v);
        if (p[i] != v) return false;
      }
      return true;
    };
    uint8_t pmk[32];
    CHECK(crypto::pmk_from_passphrase("password", "IEEE", pmk) &&
              hex_eq(pmk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"),
          "PBKDF2 PMK matches IEEE 802.11i H.4 (\"password\"/\"IEEE\")");
    uint8_t key20[20];
    std::memset(key20, 0x0b, 20);
    uint8_t prf[64];
    const char *hi = "Hi There";
    CHECK(crypto::prf_sha1(key20, 20, "prefix", reinterpret_cast<const uint8_t *>(hi), 8, prf, 64) &&
              hex_eq(prf, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                          "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"),
          "PRF-512 matches IEEE 802.11i H.3 test 1");
    uint8_t kek[16], plain[16];
    for (int i = 0; i < 16; i++) {
      kek[i] = static_cast<uint8_t>(i);
      plain[i] = static_cast<uint8_t>(0x11 * i);
    }
    std::vector<uint8_t> wrapped, back;
    CHECK(crypto::aes_wrap(kek, 16, plain, 16, wrapped) &&
              hex_eq(wrapped.data(), "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"),
          "AES key wrap matches RFC 3394 4.1");
    CHECK(crypto::aes_unwrap(kek, 16, wrapped.data(), wrapped.size(), back) && back.size() == 16 &&
              std::memcmp(back.data(), plain, 16) == 0,
          "AES key unwrap round-trips");
    wrapped[3] ^= 1;
    CHECK(!crypto::aes_unwrap(kek, 16, wrapped.data(), wrapped.size(), back),
          "AES key unwrap rejects a corrupted key");

    /* WPA3: AES-CMAC (RFC 4493), P-256, SAE (IEEE 802.11-2020 J.10, as hostap's sae_tests), OWE. */
    auto unhex = [](const char *h, uint8_t *out) {
      for (size_t i = 0; h[2 * i]; i++) {
        unsigned b;
        std::sscanf(h + 2 * i, "%2x", &b);
        out[i] = static_cast<uint8_t>(b);
      }
    };
    uint8_t ck[16], cm[64], tag[16];
    unhex("2b7e151628aed2a6abf7158809cf4f3c", ck);
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
          "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", cm);
    CHECK(wpa3::aes_cmac(ck, cm, 0, tag) && hex_eq(tag, "bb1d6929e95937287fa37d129b756746"), "AES-CMAC RFC 4493 (empty)");
    CHECK(wpa3::aes_cmac(ck, cm, 40, tag) && hex_eq(tag, "dfa66747de9ae63030ca32611497c827"), "AES-CMAC RFC 4493 (40 bytes)");
    CHECK(wpa3::aes_cmac(ck, cm, 64, tag) && hex_eq(tag, "51f0bebf7e3b9d92fc49741779363cfe"), "AES-CMAC RFC 4493 (64 bytes)");
    uint8_t two[32] = {}, g2[64];
    two[31] = 2;
    CHECK(wpa3::p256_mul_base(two, g2) &&
              hex_eq(g2, "7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978"
                         "07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1"),
          "P-256 2G");
    const uint8_t sa1[6] = {0x4d, 0x3f, 0x2f, 0xff, 0xe3, 0x87}, sa2[6] = {0xa5, 0xd8, 0xaa, 0x95, 0x8e, 0x3c};
    uint8_t srand[32], smask[32], peer[98];
    unhex("992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94", srand);
    unhex("9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322", smask);
    unhex(
        "1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b"
        "5223e71b9bb048d3873f20556953a96c91536fd8ee6ca9b4a68a148b056a909b"
        "e03e83ae208f60f8ef5537858074db06687032399862999b511e0a1552a5fea3"
        "17c2",
        peer);
    wpa3::Sae sae;
    const bool sae_ok = sae.prepare(sa1, sa2, "mekmitasdigoat") && sae.commit(srand, smask);
    const std::vector<uint8_t> body = sae.commit_body({});
    CHECK(sae_ok && body.size() == 98 &&
              hex_eq(body.data(),
        "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c257"
        "5c65d5ad9e00829707aa36ba8b859738fc961d08243505f47c035376d7ac4bc8"
        "d7b95083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e1272621325"
        "dbe1"),
          "SAE commit matches IEEE 802.11-2020 J.10");
    CHECK(sae.process_commit(peer, sizeof(peer)) &&
              hex_eq(sae.kck(), "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a") &&
              hex_eq(sae.pmk(), "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59") &&
              hex_eq(sae.pmkid(), "8747a600eea3f9f22475df58ca1e5498"),
          "SAE KCK, PMK and PMKID match J.10");
    CHECK(!sae.process_commit(body.data(), body.size()), "SAE refuses a reflected commit");
    {
      wpa3::Sae a, b;
      bool ok = a.prepare(sa1, sa2, "correct horse") && b.prepare(sa2, sa1, "correct horse") && a.commit() && b.commit();
      const auto ba = a.commit_body({}), bb = b.commit_body({});
      uint8_t ca[32], cb[32];
      ok = ok && a.process_commit(bb.data(), bb.size()) && b.process_commit(ba.data(), ba.size()) && a.confirm(1, ca) &&
           b.confirm(1, cb);
      CHECK(ok && std::memcmp(a.pmk(), b.pmk(), 32) == 0 && a.verify(1, cb) && b.verify(1, ca),
            "SAE peers with random values agree and confirm");
    }
    wpa3::Owe owe_a, owe_b;
    CHECK(owe_a.generate() && owe_b.generate() && owe_a.derive(owe_b.public_x()), "OWE key agreement");
  }

  /* Radio backends: capability matrix, per-capability policies, and a
   * station-less backend meeting the contract without stubs. */
  failures += radio_backend_test::run();

  /* Out-of-order completion: entries behind an older frame must not go back
   * into the hardware window, regardless of their previous retry deadline. */
  {
    awdl::UnicastRetry head, acked, failed;
    head.slot = 4;
    head.attempts = 1;
    head.first_us = head.sent_us = 100;
    acked.acked = true;
    failed.abandon = true;
    CHECK(!head.ready(101) && !acked.ready(101) && !failed.ready(101),
          "out-of-order completions stay out of the TX window");
    CHECK(!head.status_missing(100 + awdl::kTxStatusTimeoutUS - 1) &&
              head.status_missing(100 + awdl::kTxStatusTimeoutUS) &&
              head.expired(100 + awdl::kTxStatusTimeoutUS),
          "missing TX completion is recognized after the retry window");
    head.slot = -1;
    head.next_us = 2100;
    CHECK(!head.ready(2099) && head.ready(2100) && !head.expired(2100),
          "an unconfirmed frame still retries within its backoff and window");
    CHECK(head.expired(100 + awdl::kResendWindowUS),
          "expired retries cannot be resubmitted after a long pause");
    head.attempts = awdl::kResendMaxAttempts;
    CHECK(head.expired(2100), "unicast also bounds the number of retries");
  }

  /* Experimental A-MPDU send fallback (#4): trip only after two full windows
   * with zero Block Acks and >= 64 single-frame completions. */
  CHECK(!awdl::ba_tx_stalled_as_singles(127, 0, 127), "no-BA fallback waits for 128 frames");
  CHECK(!awdl::ba_tx_stalled_as_singles(128, 0, 63), "no-BA fallback waits for 64 single completions");
  CHECK(!awdl::ba_tx_stalled_as_singles(256, 1, 200), "no-BA fallback stays quiet once any Block Ack arrives");
  CHECK(awdl::ba_tx_stalled_as_singles(128, 0, 64), "no-BA fallback triggers on 128 frames / 0 BA / 64 singles");

  /* Never disable A-MPDU sending for the rest of the radio's life: a first
   * stall retries after a minute, later ones after five, and a session that did
   * produce Block Acks clears the count (#39). */
  CHECK(awdl::ba_tx_retry_us(1, 1000) == 1000 + awdl::kBaTxFailBackoffUS,
        "a first BA stall retries after the short backoff");
  CHECK(awdl::ba_tx_retry_us(2, 1000) == 1000 + awdl::kBaTxLongBackoffUS,
        "the second BA stall backs off for the long interval");
  CHECK(awdl::ba_tx_retry_us(9, 1000) == 1000 + awdl::kBaTxLongBackoffUS,
        "later BA stalls keep the long backoff, never forever");
  CHECK(awdl::kBaTxLongBackoffUS > awdl::kBaTxFailBackoffUS,
        "the long backoff is longer than the short one");

  /* Host TX queue policy: no station frame may leave the host while the
   * station is yielded to AWDL or paused, and a stranded queue must not keep
   * the radio main loop busy after the link leaves Running. */
  {
    CHECK(!sta_tx::can_send(false, false, false), "TX policy: a station that is not Running cannot send");
    CHECK(!sta_tx::can_send(true, true, false), "TX policy: a yielded station cannot send");
    CHECK(!sta_tx::can_send(true, false, true), "TX policy: a paused station cannot send");
    CHECK(sta_tx::can_send(true, false, false), "TX policy: a running, unyielded station can send");
    CHECK(!sta_tx::pending(false, false, false, true, 1), "TX policy: not Running is not busy");
    CHECK(!sta_tx::pending(true, true, false, true, 1), "TX policy: yielded queue is not busy");
    CHECK(!sta_tx::pending(true, false, true, true, 1), "TX policy: paused queue is not busy");
    CHECK(sta_tx::pending(true, false, false, true, 0), "TX policy: host queue is busy");
    CHECK(sta_tx::pending(true, false, false, false, 1), "TX policy: firmware queue is busy");
  }

  /* Intel station RX Block Ack reordering (PR #93). */
  {
    iwx::RxBaReorder ba;
    const uint8_t ap[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    const uint8_t me[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::vector<uint16_t> out;
    const auto emit = [&](const RxFrame &f) {
      const uint16_t sc = static_cast<uint16_t>(f.data[22] | (f.data[23] << 8));
      out.push_back(static_cast<uint16_t>((sc >> 4) & 0x0fffu));
    };
    const auto make_qos = [&](uint16_t seq, uint8_t tid) {
      std::vector<uint8_t> buf(32, 0);
      buf[0] = 0x88; /* QoS Data */
      std::memcpy(buf.data() + 4, me, 6);
      std::memcpy(buf.data() + 10, ap, 6);
      const uint16_t sc = static_cast<uint16_t>(seq << 4);
      buf[22] = static_cast<uint8_t>(sc);
      buf[23] = static_cast<uint8_t>(sc >> 8);
      buf[24] = tid & 0x0fu;
      return buf;
    };
    ba.on_addba(ap, 0, 10, 64, 1000, emit);
    auto f10 = make_qos(10, 0), f12 = make_qos(12, 0), f11 = make_qos(11, 0), f14 = make_qos(14, 0);
    auto wrap = [](const std::vector<uint8_t> &b) {
      RxFrame r{};
      r.data = b.data();
      r.len = b.size();
      return r;
    };
    CHECK(ba.process(wrap(f10), 1100, emit) && out.size() == 1 && out[0] == 10, "STA RX BA emits in-order SSN immediately");
    CHECK(ba.process(wrap(f12), 1200, emit) && out.size() == 1, "STA RX BA buffers out-of-order seq 12");
    CHECK(ba.process(wrap(f12), 1250, emit) && out.size() == 1, "STA RX BA suppresses duplicate seq 12");
    CHECK(ba.process(wrap(f11), 1300, emit) && out.size() == 3 && out[1] == 11 && out[2] == 12,
          "STA RX BA drains 11 and 12 in order once 11 arrives");
    CHECK(ba.process(wrap(f14), 2000, emit) && out.size() == 3, "STA RX BA buffers seq 14 with gap at 13");
    ba.flush_expired(2000 + iwx::RxBaReorder::kStallTimeoutUs + 1, emit);
    CHECK(out.size() == 4 && out[3] == 14, "STA RX BA flushes stalled seq 14 after 40 ms timeout");
    ba.on_delba(ap, 0, emit);
  }

  /* Experimental Wi-Fi 6 station: our HE Capabilities, the AP's elements and
   * what the firmware gets from them. */
  {
    uint8_t mine[sta_he::kOurCapElementSize];
    CHECK(sta_he::our_cap_element(mine, true) == 24 && mine[0] == 255 && mine[1] == 22 && mine[2] == 35 &&
              mine[3] == 0x01 && (mine[10] & 0x20) && mine[20] == 0xfa && mine[21] == 0xff,
          "HE: our capabilities element (HTC-HE, LDPC, MCS 0-11 on two streams)");
    sta_he::our_cap_element(mine, false);
    CHECK(mine[20] == 0xfe && mine[22] == 0xfe, "HE: one-stream map when the card has one chain");

    /* PPE thresholds: NSS field 1 (two streams), RU 242 and 484; PPET16, PPET8 per pair. */
    const unsigned vals[4][2] = {{1, 2}, {3, 4}, {5, 6}, {0, 7}};
    uint8_t ppe[4] = {0x19, 0, 0, 0};
    unsigned bit = 7;
    for (const auto &v : vals)
      for (unsigned k = 0; k < 2; k++, bit += 3)
        for (unsigned b = 0; b < 3; b++)
          if ((v[k] >> b) & 1) ppe[(bit + b) / 8] |= static_cast<uint8_t>(1u << ((bit + b) % 8));
    std::vector<uint8_t> cap = {0x09, 0, 0x28, 0, 0, 0,                    /* MAC: HTC-HE, BSR, 32-bit BA */
                                0x04, 0x20, 0x08, 0, 0, 0, 0x80, 0, 0, 0, 0, /* PHY: 80 MHz, LDPC, STBC RX, PPE */
                                0xfa, 0xff, 0xfa, 0xff};
    cap.insert(cap.end(), ppe, ppe + 4);
    sta_he::ApHe ap;
    CHECK(sta_he::ppe_size(0x19, cap.data() + 6) == 4, "HE: PPE threshold size for 2 streams x 2 RUs");
    CHECK(sta_he::parse_cap(cap.data(), cap.size(), ap) && ap.cap && ap.ppe_len == 4 && ap.rx_mcs80 == 0xfffa &&
              ap.ldpc(),
          "HE: AP capabilities parsed");
    CHECK(!sta_he::parse_cap(cap.data(), cap.size() - 1, ap), "HE: truncated PPE thresholds rejected");
    sta_he::parse_cap(cap.data(), cap.size(), ap);
    const uint8_t op[6] = {0x44, 0x00, 0x00, 0x05, 0xfc, 0xff}; /* PE 4, RTS 4 (x32 us), color 5 */
    CHECK(sta_he::parse_op(op, sizeof(op), ap) && ap.bss_color() == 5 &&
              (ap.op_params & sta_he::kOpPeDurationMask) == 4 &&
              ((ap.op_params & sta_he::kOpRtsThresholdMask) >> 4) == 4,
          "HE: AP operation parsed (PE duration, RTS threshold, BSS color)");
    const uint8_t op_vht[6] = {0x00, 0x40, 0x00, 0x01, 0xfc, 0xff};
    sta_he::ApHe ap2;
    CHECK(!sta_he::parse_op(op_vht, sizeof(op_vht), ap2), "HE: operation missing its VHT info rejected");
    const uint8_t edca[13] = {0, 0x03, 0xa4, 8, 0x27, 0xa4, 8, 0x42, 0x43, 8, 0x62, 0x32, 8};
    CHECK(sta_he::parse_mu_edca(edca, sizeof(edca), ap) && ap.mu_edca && ap.mu_edca_rec[1] == 0xa4,
          "HE: MU EDCA records parsed");
    CHECK(sta_he::mu_edca_fw_ac(0) == 1 && sta_he::mu_edca_fw_ac(1) == 0 && sta_he::mu_edca_fw_ac(3) == 3,
          "HE: MU EDCA BE/BK/VI/VO map to firmware ACs");

    uint8_t pe[2][4][2];
    CHECK(sta_he::packet_ext(ap, &pe[0][0][0], 4) && pe[0][0][1] == 1 && pe[0][0][0] == 2 && pe[0][1][1] == 3 &&
              pe[0][1][0] == 4 && pe[1][0][1] == 5 && pe[1][0][0] == 6 && pe[1][1][1] == 0 && pe[1][1][0] == 7 &&
              pe[0][2][0] == 7 && pe[1][3][1] == 7,
          "HE: packet extension from PPE thresholds");
    sta_he::ApHe pad = ap;
    pad.phy[6] = 0;
    pad.ppe_len = 0;
    pad.phy[9] = 0x80; /* nominal padding 16 us */
    CHECK(sta_he::packet_ext(pad, &pe[0][0][0], 4) && pe[0][0][0] == 7 && pe[0][0][1] == 0 && pe[1][3][1] == 0,
          "HE: packet extension from nominal padding");
    pad.phy[9] = 0xc0;
    CHECK(!sta_he::packet_ext(pad, &pe[0][0][0], 4), "HE: reserved padding means no packet extension");
    uint8_t pe5[2][5][2];
    CHECK(sta_he::packet_ext(ap, &pe5[0][0][0], 5) && pe5[0][1][0] == 4 && pe5[1][1][1] == 0 && pe5[0][4][0] == 7,
          "HE: packet extension in the MLD layout (5 RU sizes)");

    CHECK(sta_he::tlc_mask(0xfffa, 0, true) == 0x0fff && sta_he::tlc_mask(0xfffa, 1, true) == 0x0fff,
          "HE: TLC MCS 0-11 on both streams");
    CHECK(sta_he::tlc_mask(0xfffa, 0, false) == 0x03ff, "HE: no MCS 10/11 without LDPC");
    CHECK(sta_he::tlc_mask(0xfff4, 0, true) == 0x00ff && sta_he::tlc_mask(0xfff4, 1, true) == 0x03ff,
          "HE: TLC follows the AP's per-stream map");
    CHECK(sta_he::tlc_mask(0xfffe, 1, true) == 0 && sta_he::ap_streams(0xfffe) == 1 && sta_he::ap_streams(0xfffa) == 2,
          "HE: one-stream AP");
    CHECK(sta_he::htc_flags(ap) == (1u | 1u << 4), "HE: HT Control flags (HTC-HE, BSR)");
  }

  /* Station MAC translation at the TAP boundary (#96): ARP, DHCPv4, IPv6 NDP. */
  {
    const uint8_t tap_mac[6] = {0x00, 0xff, 0xb1, 0xc5, 0x24, 0xcc};
    const uint8_t win_mac[6] = {0xa4, 0x6b, 0xb6, 0x12, 0x34, 0x56}; /* universal OUI (bit 1 == 0) */
    uint8_t arp[28] = {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01};
    std::memcpy(arp + 8, tap_mac, 6);
    CHECK(sta_mac_nat::translate_payload(0x0806, arp, sizeof(arp), tap_mac, win_mac, true) &&
              std::memcmp(arp + 8, win_mac, 6) == 0,
          "MAC NAT: ARP TX rewrites sender hardware address");
    std::memcpy(arp + 18, win_mac, 6);
    CHECK(sta_mac_nat::translate_payload(0x0806, arp, sizeof(arp), win_mac, tap_mac, false) &&
              std::memcmp(arp + 18, tap_mac, 6) == 0,
          "MAC NAT: ARP RX rewrites target hardware address");

    /* IPv4 DHCP Discover (20 IPv4 + 8 UDP + 250 BOOTP/options). */
    std::vector<uint8_t> dhcp(20 + 8 + 250, 0);
    dhcp[0] = 0x45;
    dhcp[9] = 17;
    dhcp[16] = 255; dhcp[17] = 255; dhcp[18] = 255; dhcp[19] = 255;
    const uint16_t ulen = 8 + 250;
    dhcp[20] = 0; dhcp[21] = 68; dhcp[22] = 0; dhcp[23] = 67;
    dhcp[24] = static_cast<uint8_t>(ulen >> 8); dhcp[25] = static_cast<uint8_t>(ulen);
    dhcp[26] = 0x12; dhcp[27] = 0x34; /* non-zero so checksum is recomputed */
    uint8_t *bp = dhcp.data() + 28;
    bp[0] = 1; bp[1] = 1; bp[2] = 6;
    std::memcpy(bp + 28, tap_mac, 6);
    bp[236] = 0x63; bp[237] = 0x82; bp[238] = 0x53; bp[239] = 0x63;
    bp[240] = 61; bp[241] = 7; bp[242] = 1;
    std::memcpy(bp + 243, tap_mac, 6);
    CHECK(sta_mac_nat::translate_payload(0x0800, dhcp.data(), dhcp.size(), tap_mac, win_mac, true) &&
              std::memcmp(bp + 28, win_mac, 6) == 0 && std::memcmp(bp + 243, win_mac, 6) == 0,
          "MAC NAT: DHCPv4 TX rewrites chaddr and Option 61");
    uint32_t sum = sta_mac_nat::csum_add(0, dhcp.data() + 12, 8);
    sum += 17u + ulen;
    sum = sta_mac_nat::csum_add(sum, dhcp.data() + 20, ulen);
    CHECK(sta_mac_nat::csum_fold(sum) == 0, "MAC NAT: DHCPv4 TX UDP checksum valid");

    /* IPv6 NDP Neighbor Solicitation (40 IPv6 + 32 ICMPv6). */
    std::vector<uint8_t> ndp(40 + 32, 0);
    ndp[0] = 0x60;
    ndp[5] = 32;
    ndp[6] = 58;
    ndp[8] = 0xfe; ndp[9] = 0x80;
    ndp[24] = 0xff; ndp[25] = 0x02;
    ndp[40] = 135; /* NS */
    ndp[64] = 1; ndp[65] = 1; /* SLLA */
    std::memcpy(ndp.data() + 66, tap_mac, 6);
    CHECK(sta_mac_nat::translate_payload(0x86dd, ndp.data(), ndp.size(), tap_mac, win_mac, true) &&
              std::memcmp(ndp.data() + 66, win_mac, 6) == 0,
          "MAC NAT: IPv6 NDP rewrites SLLA option");
    uint32_t sum6 = sta_mac_nat::csum_add(0, ndp.data() + 8, 32);
    sum6 += 32u + 58u;
    sum6 = sta_mac_nat::csum_add(sum6, ndp.data() + 40, 32);
    CHECK(sta_mac_nat::csum_fold(sum6) == 0, "MAC NAT: IPv6 NDP ICMPv6 checksum valid");
  }

  std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
