/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "pdcp_entity_tx.h"
#include "../support/sdu_window_impl.h"
#include "srsran/instrumentation/traces/up_traces.h"
#include "srsran/security/ciphering.h"
#include "srsran/security/integrity.h"
#include "srsran/support/bit_encoding.h"
#include "srsran/support/srsran_assert.h"

using namespace srsran;

/// \brief Receive an SDU from the upper layers, apply encryption
/// and integrity protection and pass the resulting PDU
/// to the lower layers.
///
/// \param sdu Buffer that hold the SDU from higher layers.
/// \ref TS 38.323 section 5.2.1: Transmit operation
void pdcp_entity_tx::handle_sdu(byte_buffer sdu)
{
  trace_point tx_tp = up_tracer.now();
  // Avoid TX'ing if we are close to overload RLC SDU queue
  if (st.tx_trans > st.tx_next) {
    logger.log_error("Invalid state, tx_trans is larger than tx_next. {}", st);
    return;
  }
  if ((st.tx_next - st.tx_trans) >= cfg.custom.rlc_sdu_queue) {
    logger.log_info("Dropping SDU to avoid overloading RLC queue. rlc_sdu_queue={} {}", cfg.custom.rlc_sdu_queue, st);
    return;
  }
  if ((st.tx_next - st.tx_trans) >= (window_size - 1)) {
    logger.log_info("Dropping SDU to avoid going over the TX window size. {}", st);
    return;
  }

  metrics_add_sdus(1, sdu.length());
  logger.log_debug(sdu.begin(), sdu.end(), "TX SDU. sdu_len={}", sdu.length());

  // The PDCP is not allowed to use the same COUNT value more than once for a given security key,
  // see TS 38.331, section 5.3.1.2. To avoid this, we notify the RRC once we exceed a "maximum"
  // COUNT. It is then the RRC's responsibility to refresh the keys. We continue transmitting until
  // we reached a maximum hard COUNT, after which we simply refuse to TX any further.
  if (st.tx_next >= cfg.custom.max_count.hard) {
    if (!max_count_overflow) {
      logger.log_error("Reached maximum count, refusing to transmit further. count={}", st.tx_next);
      upper_cn.on_protocol_failure();
      max_count_overflow = true;
    }
    return;
  }
  if (st.tx_next >= cfg.custom.max_count.notify) {
    if (!max_count_notified) {
      logger.log_warning("Approaching count wrap-around, notifying RRC. count={}", st.tx_next);
      upper_cn.on_max_count_reached();
      max_count_notified = true;
    }
  }

  // Perform header compression
  // TODO

  // Prepare header
  pdcp_data_pdu_header hdr = {};
  hdr.sn                   = SN(st.tx_next);

  // Pack header
  byte_buffer header_buf = {};
  if (not write_data_pdu_header(header_buf, hdr)) {
    logger.log_error("Could not append PDU header, dropping SDU and notifying RRC. count={}", st.tx_next);
    upper_cn.on_protocol_failure();
    return;
  }

  // Apply ciphering and integrity protection
  expected<byte_buffer> exp_buf =
      apply_ciphering_and_integrity_protection(std::move(header_buf), sdu.deep_copy(), st.tx_next);
  if (exp_buf.is_error()) {
    logger.log_error("Could not apply ciphering and integrity protection, dropping SDU and notifying RRC. count={}",
                     st.tx_next);
    upper_cn.on_protocol_failure();
    return;
  }
  byte_buffer protected_buf = std::move(exp_buf.value());

  // Create a discard timer and put into tx_window. For AM, also store the SDU for a possible data recovery procedure.
  if (cfg.discard_timer.has_value()) {
    unique_timer discard_timer = {};
    // Only start for finite durations
    if (cfg.discard_timer.value() != pdcp_discard_timer::infinity) {
      discard_timer = timers.create_timer();
      discard_timer.set(std::chrono::milliseconds(static_cast<unsigned>(cfg.discard_timer.value())),
                        discard_callback{this, st.tx_next});
      discard_timer.run();
    }

    // If the place in the tx_window is occupied by an old element from previous wrap, discard that element first.
    if (tx_window->has_sn(st.tx_next)) {
      uint32_t old_count = (*tx_window)[st.tx_next].count;
      logger.log_error("Tx window full. Discarding old_count={}. tx_next={}", old_count, st.tx_next);
      discard_pdu(old_count);
    }

    pdcp_tx_sdu_info& sdu_info = tx_window->add_sn(st.tx_next);
    sdu_info.count             = st.tx_next;
    sdu_info.discard_timer     = std::move(discard_timer);
    if (is_am()) {
      sdu_info.sdu = std::move(sdu);
    }
    logger.log_debug("Added to tx window. count={} discard_timer={}", st.tx_next, cfg.discard_timer);
  }

  // Write to lower layers
  write_data_pdu_to_lower_layers(st.tx_next, std::move(protected_buf));

  // Increment TX_NEXT
  st.tx_next++;

  up_tracer << trace_event{"pdcp_tx_pdu", tx_tp};
}

void pdcp_entity_tx::reestablish(security::sec_128_as_config sec_cfg_)
{
  // - for UM DRBs and AM DRBs, reset the ROHC protocol for uplink and start with an IR state in U-mode (as
  //   defined in RFC 3095 [8] and RFC 4815 [9]) if drb-ContinueROHC is not configured in TS 38.331 [3];
  // - for UM DRBs and AM DRBs, reset the EHC protocol for uplink if drb-ContinueEHC-UL is not configured in
  //   TS 38.331 [3];
  //   Header compression not supported yet (TODO).

  // - for UM DRBs and SRBs, set TX_NEXT to the initial value;
  // - for SRBs, discard all stored PDCP SDUs and PDCP PDUs;
  if (is_srb() || is_um()) {
    reset(); // While not explicitly stated in the spec,
             // there is no point in storing PDCP UM PDUs.
             // They cannot be RETXed and RLC already discarded them.
             // Also, this avoids having multiple discard timers
             // associated with the with the same COUNT.
  }

  // - apply the ciphering algorithm and key provided by upper layers during the PDCP entity re-establishment
  //   procedure;
  // - apply the integrity protection algorithm and key provided by upper layers during the PDCP entity re-
  //   establishment procedure;
  configure_security(sec_cfg_);

  // - for UM DRBs, for each PDCP SDU already associated with a PDCP SN but for which a corresponding PDU has
  //   not previously been submitted to lower layers, and;
  // - for AM DRBs for Uu interface whose PDCP entities were suspended, from the first PDCP SDU for which the
  //   successful delivery of the corresponding PDCP Data PDU has not been confirmed by lower layers, for each
  //   PDCP SDU already associated with a PDCP SN:
  //   - consider the PDCP SDUs as received from upper layer;
  //   - perform transmission of the PDCP SDUs in ascending order of the COUNT value associated to the PDCP
  //     SDU prior to the PDCP re-establishment without restarting the discardTimer, as specified in clause 5.2.1;
  //
  //  For UM DRBs, when SDUs are associated with a PDCP SN they are immediately pushed to the lower-layer.
  //  As such, there is nothing to do here.
  //  For AM DRBs, PDCP entity suspension is not supported yet (TODO).

  // - for AM DRBs whose PDCP entities were not suspended, from the first PDCP SDU for which the successful
  //   delivery of the corresponding PDCP Data PDU has not been confirmed by lower layers, perform retransmission
  //   or transmission of all the PDCP SDUs already associated with PDCP SNs in ascending order of the COUNT
  //   values associated to the PDCP SDU prior to the PDCP entity re-establishment as specified below:
  //   - perform header compression of the PDCP SDU using ROHC as specified in the clause 5.7.4 and/or using
  //     EHC as specified in the clause 5.12.4;
  //   - perform integrity protection and ciphering of the PDCP SDU using the COUNT value associated with this
  //     PDCP SDU as specified in the clause 5.9 and 5.8;
  //   - submit the resulting PDCP Data PDU to lower layer, as specified in clause 5.2.1.
  if (is_am()) {
    retransmit_all_pdus();
  }
}

void pdcp_entity_tx::write_data_pdu_to_lower_layers(uint32_t count, byte_buffer buf)
{
  logger.log_info(
      buf.begin(), buf.end(), "TX PDU. type=data pdu_len={} sn={} count={}", buf.length(), SN(count), count);
  metrics_add_pdus(1, buf.length());
  pdcp_tx_pdu tx_pdu = {};
  tx_pdu.buf         = std::move(buf);
  if (is_drb()) {
    tx_pdu.pdcp_sn = SN(count); // Set only for data PDUs on DRBs.
  }
  lower_dn.on_new_pdu(std::move(tx_pdu));
}

void pdcp_entity_tx::write_control_pdu_to_lower_layers(byte_buffer buf)
{
  logger.log_info(buf.begin(), buf.end(), "TX PDU. type=ctrl pdu_len={}", buf.length());
  metrics_add_pdus(1, buf.length());
  pdcp_tx_pdu tx_pdu = {};
  tx_pdu.buf         = std::move(buf);
  // tx_pdu.pdcp_sn is not set for control PDUs
  lower_dn.on_new_pdu(std::move(tx_pdu));
}

void pdcp_entity_tx::handle_status_report(byte_buffer_chain status)
{
  byte_buffer buf = {status.begin(), status.end()};
  bit_decoder dec(buf);

  // Unpack and check PDU header
  uint32_t dc = 0;
  dec.unpack(dc, 1);
  if (dc != to_number(pdcp_dc_field::control)) {
    logger.log_warning(
        buf.begin(), buf.end(), "Invalid D/C field in status report. dc={}", to_number(pdcp_dc_field::control), dc);
    return;
  }
  uint32_t cpt = 0;
  dec.unpack(cpt, 3);
  if (cpt != to_number(pdcp_control_pdu_type::status_report)) {
    logger.log_warning(buf.begin(),
                       buf.end(),
                       "Invalid CPT field in status report. cpt={}",
                       to_number(pdcp_control_pdu_type::status_report),
                       cpt);
    return;
  }
  uint32_t reserved = 0;
  dec.unpack(reserved, 4);
  if (reserved != 0) {
    logger.log_warning(
        buf.begin(), buf.end(), "Ignoring status report because reserved bits are set. reserved={:#x}", reserved);
    return;
  }

  // Unpack FMC field
  uint32_t fmc = 0;
  dec.unpack(fmc, 32);
  logger.log_info("Status report. fmc={}", fmc);

  // Discard any SDU with COUNT < FMC
  for (uint32_t count = st.tx_next_ack; count < fmc; count++) {
    discard_pdu(count);
  }

  // Evaluate bitmap: discard any SDU with the bit in the bitmap set to 1
  unsigned bit = 0;
  while (dec.unpack(bit, 1)) {
    fmc++;
    // Bit == 0: PDCP SDU with COUNT = (FMC + bit position) modulo 2^32 is missing.
    // Bit == 1: PDCP SDU with COUNT = (FMC + bit position) modulo 2^32 is correctly received.
    if (bit == 1) {
      discard_pdu(fmc);
    }
  }
}

/*
 * Ciphering and Integrity Protection Helpers
 */
expected<byte_buffer>
pdcp_entity_tx::apply_ciphering_and_integrity_protection(byte_buffer hdr, byte_buffer sdu, uint32_t count)
{
  // TS 38.323, section 5.9: Integrity protection
  // The data unit that is integrity protected is the PDU header
  // and the data part of the PDU before ciphering.
  security::sec_mac mac = {};
  if (integrity_enabled == security::integrity_enabled::on) {
    byte_buffer buf = {};
    if (not buf.append(hdr)) {
      return default_error_t{};
    }
    if (not buf.append(sdu)) {
      return default_error_t{};
    }
    integrity_generate(mac, buf, count);
  }

  // TS 38.323, section 5.8: Ciphering
  // The data unit that is ciphered is the MAC-I and the
  // data part of the PDCP Data PDU except the
  // SDAP header and the SDAP Control PDU if included in the PDCP SDU.
  byte_buffer ct;
  if (ciphering_enabled == security::ciphering_enabled::on &&
      sec_cfg.cipher_algo != security::ciphering_algorithm::nea0) {
    // Append MAC-I
    if (is_srb() || (is_drb() && (integrity_enabled == security::integrity_enabled::on))) {
      if (not sdu.append(mac)) {
        return default_error_t{};
      }
    }
    cipher_encrypt(sdu, count);
    ct = std::move(sdu);
  } else {
    ct = std::move(sdu);

    // Append MAC-I
    if (is_srb() || (is_drb() && (integrity_enabled == security::integrity_enabled::on))) {
      if (not ct.append(mac)) {
        return default_error_t{};
      }
    }
  }

  // Construct the protected buffer
  byte_buffer protected_buf;
  if (not protected_buf.append(std::move(hdr))) {
    return default_error_t{};
  }
  if (not protected_buf.append(std::move(ct))) {
    return default_error_t{};
  }

  return protected_buf;
}

void pdcp_entity_tx::integrity_generate(security::sec_mac& mac, byte_buffer_view buf, uint32_t count)
{
  srsran_assert(sec_cfg.k_128_int.has_value(), "Cannot generate integrity: Integrity key is not configured.");
  srsran_assert(sec_cfg.integ_algo.has_value(), "Cannot generate integrity: Integrity algorithm is not configured.");
  switch (sec_cfg.integ_algo.value()) {
    case security::integrity_algorithm::nia0:
      // TS 33.501, Sec. D.1
      // The NIA0 algorithm shall be implemented in such way that it shall generate a 32 bit MAC-I/NAS-MAC and
      // XMAC-I/XNAS-MAC of all zeroes (see sub-clause D.3.1).
      std::fill(mac.begin(), mac.end(), 0);
      break;
    case security::integrity_algorithm::nia1:
      security_nia1(mac, sec_cfg.k_128_int.value(), count, bearer_id, direction, buf.begin(), buf.end());
      break;
    case security::integrity_algorithm::nia2:
      security_nia2(mac, sec_cfg.k_128_int.value(), count, bearer_id, direction, buf.begin(), buf.end());
      break;
    case security::integrity_algorithm::nia3:
      security_nia3(mac, sec_cfg.k_128_int.value(), count, bearer_id, direction, buf.begin(), buf.end());
      break;
    default:
      break;
  }

  logger.log_debug("Integrity gen. count={} bearer_id={} dir={}", count, bearer_id, direction);
  logger.log_debug((uint8_t*)sec_cfg.k_128_int.value().data(), sec_cfg.k_128_int.value().size(), "Integrity gen key.");
  logger.log_debug(buf.begin(), buf.end(), "Integrity gen input message.");
  logger.log_debug((uint8_t*)mac.data(), mac.size(), "MAC generated.");
}

void pdcp_entity_tx::cipher_encrypt(byte_buffer& buf, uint32_t count)
{
  logger.log_debug("Cipher encrypt. count={} bearer_id={} dir={}", count, bearer_id, direction);
  logger.log_debug((uint8_t*)sec_cfg.k_128_enc.data(), sec_cfg.k_128_enc.size(), "Cipher encrypt key.");
  logger.log_debug(buf.begin(), buf.end(), "Cipher encrypt input msg.");

  switch (sec_cfg.cipher_algo) {
    case security::ciphering_algorithm::nea1:
      security_nea1(sec_cfg.k_128_enc, count, bearer_id, direction, buf);
      break;
    case security::ciphering_algorithm::nea2:
      security_nea2(sec_cfg.k_128_enc, count, bearer_id, direction, buf);
      break;
    case security::ciphering_algorithm::nea3:
      security_nea3(sec_cfg.k_128_enc, count, bearer_id, direction, buf);
      break;
    default:
      break;
  }
  logger.log_debug(buf.begin(), buf.end(), "Cipher encrypt output msg.");
}

/*
 * Status report and data recovery
 */
void pdcp_entity_tx::send_status_report()
{
  if (cfg.status_report_required) {
    logger.log_info("Status report triggered.");
    byte_buffer status_report = status_provider->compile_status_report();
    write_control_pdu_to_lower_layers(std::move(status_report));
  } else {
    logger.log_warning("Status report triggered but not configured.");
  }
}

void pdcp_entity_tx::data_recovery()
{
  srsran_assert(is_drb() && cfg.rlc_mode == pdcp_rlc_mode::am, "Invalid bearer type for data recovery.");
  logger.log_info("Data recovery requested.");

  /*
   * TS 38.323 Sec. 5.4.1:
   * [...] the receiving PDCP entity shall trigger a PDCP status report when:
   * [...] -upper layer requests a PDCP data recovery; [...]
   */
  if (cfg.status_report_required) {
    send_status_report();
  }
  retransmit_all_pdus();
}

void pdcp_entity_tx::reset()
{
  st = {};
  tx_window->clear();
  logger.log_debug("Entity was reset. {}", st);
}

void pdcp_entity_tx::retransmit_all_pdus()
{
  if (!cfg.discard_timer.has_value()) {
    logger.log_debug("Cannot retransmit. No discard timer configured.");
    return;
  }
  if (!is_am()) {
    logger.log_error("Cannot retransmit. Not an AM bearer.");
    return;
  }

  // Since we are retransmitting, rewind tx_trans to tx_next_ack
  st.tx_trans = st.tx_next_ack;

  for (uint32_t count = st.tx_next_ack; count < st.tx_next; count++) {
    if (tx_window->has_sn(count)) {
      pdcp_tx_sdu_info& sdu_info = (*tx_window)[count];

      // Prepare header
      pdcp_data_pdu_header hdr = {};
      hdr.sn                   = SN(sdu_info.count);

      // Pack header
      byte_buffer header_buf = {};
      if (not write_data_pdu_header(header_buf, hdr)) {
        logger.log_error(
            "Could not append PDU header, dropping SDU and notifying RRC. count={} {}", sdu_info.count, st);
        upper_cn.on_protocol_failure();
        return;
      }

      // Perform header compression if required
      // (TODO)

      // Perform integrity protection and ciphering
      expected<byte_buffer> exp_buf =
          apply_ciphering_and_integrity_protection(std::move(header_buf), sdu_info.sdu.deep_copy(), sdu_info.count);
      if (exp_buf.is_error()) {
        logger.log_error("Could not apply ciphering and integrity protection during retransmissions, dropping SDU and "
                         "notifying RRC. count={} {}",
                         sdu_info.count,
                         st);
        upper_cn.on_protocol_failure();
        return;
      }

      byte_buffer protected_buf = std::move(exp_buf.value());
      write_data_pdu_to_lower_layers(sdu_info.count, std::move(protected_buf));
    }
  }
}

/*
 * Notification Helpers
 */
void pdcp_entity_tx::handle_transmit_notification(uint32_t notif_sn)
{
  logger.log_debug("Handling transmit notification for notif_sn={}", notif_sn);
  if (notif_sn >= pdcp_sn_cardinality(cfg.sn_size)) {
    logger.log_error("Invalid transmit notification for notif_sn={} exceeds sn_size={}", notif_sn, cfg.sn_size);
    return;
  }
  uint32_t notif_count = notification_count_estimation(notif_sn);
  if (notif_count < st.tx_trans) {
    logger.log_error(
        "Invalid notification SN, notif_count is too low. notif_sn={} notif_count={} {}", notif_sn, notif_count, st);
    return;
  }
  if (notif_count >= st.tx_next) {
    logger.log_error(
        "Invalid notification SN, notif_count is too high. notif_sn={} notif_count={} {}", notif_sn, notif_count, st);
    return;
  }
  st.tx_trans = notif_count + 1;
  logger.log_debug("Updated tx_trans. {}", st);

  // Stop discard timers if required
  if (!cfg.discard_timer.has_value()) {
    return;
  }

  if (is_um()) {
    stop_discard_timer(notif_count);
  }
}

void pdcp_entity_tx::handle_delivery_notification(uint32_t notif_sn)
{
  logger.log_debug("Handling delivery notification for notif_sn={}", notif_sn);
  if (notif_sn >= pdcp_sn_cardinality(cfg.sn_size)) {
    logger.log_error("Invalid delivery notification for notif_sn={} exceeds sn_size={}", notif_sn, cfg.sn_size);
    return;
  }
  uint32_t notif_count = notification_count_estimation(notif_sn);
  if (notif_count >= st.tx_next) {
    logger.log_error("Got notification for invalid COUNT. notif_count={} {}", notif_count, st);
    return;
  }

  // Stop discard timers if required
  if (!cfg.discard_timer.has_value()) {
    return;
  }

  if (is_am()) {
    stop_discard_timer(notif_count);
  } else {
    logger.log_warning("Received PDU delivery notification on UM bearer. sn={}", notif_sn);
  }
}

uint32_t pdcp_entity_tx::notification_count_estimation(uint32_t notification_sn)
{
  // Get lower edge of the window. If discard timer is enabled, use the lower edge of the tx_window, i.e. TX_NEXT_ACK.
  // If discard timer is not configured, use TX_TRANS as lower edge of window.
  uint32_t tx_lower;
  if (cfg.discard_timer.has_value()) {
    tx_lower = st.tx_next_ack;
  } else {
    tx_lower = st.tx_trans;
  }

  /*
   * Calculate NOTIFICATION_COUNT. This is adapted from TS 38.331 Sec. 5.2.2 "Receive operation" of the Rx side.
   *
   * - if NOTIFICATION_SN < SN(TX_LOWER) – Window_Size:
   *   - NOTIFICATION_HFN = HFN(TX_LOWER) + 1.
   * - else if NOTIFICATION_SN >= SN(TX_LOWER) + Window_Size:
   *   - NOTIFICATION_HFN = HFN(TX_LOWER) – 1.
   * - else:
   *   - NOTIFICATION_HFN = HFN(TX_LOWER);
   * - NOTIFICATION_COUNT = [NOTIFICATION_HFN, NOTIFICATION_SN].
   */
  uint32_t notification_hfn;
  if ((int64_t)notification_sn < (int64_t)SN(tx_lower) - (int64_t)window_size) {
    notification_hfn = HFN(tx_lower) + 1;
  } else if (notification_sn >= SN(tx_lower) + window_size) {
    notification_hfn = HFN(tx_lower) - 1;
  } else {
    notification_hfn = HFN(tx_lower);
  }
  return COUNT(notification_hfn, notification_sn);
}

/*
 * PDU Helpers
 */
bool pdcp_entity_tx::write_data_pdu_header(byte_buffer& buf, const pdcp_data_pdu_header& hdr) const
{
  // Sanity check: 18-bit SN not allowed for SRBs
  srsran_assert(
      !(is_srb() && cfg.sn_size == pdcp_sn_size::size18bits), "Invalid SN size for SRB. sn_size={}", cfg.sn_size);

  byte_buffer_writer hdr_writer = buf;

  // Set D/C if required
  if (is_drb()) {
    // D/C bit field (1).
    if (not hdr_writer.append(0x80)) {
      return false;
    }
  } else {
    // No D/C bit field.
    if (not hdr_writer.append(0x00)) {
      return false;
    }
  }

  // Add SN
  switch (cfg.sn_size) {
    case pdcp_sn_size::size12bits:
      hdr_writer.back() |= (hdr.sn & 0x00000f00U) >> 8U;
      if (not hdr_writer.append((hdr.sn & 0x000000ffU))) {
        return false;
      }
      break;
    case pdcp_sn_size::size18bits:
      hdr_writer.back() |= (hdr.sn & 0x00030000U) >> 16U;
      if (not hdr_writer.append((hdr.sn & 0x0000ff00U) >> 8U)) {
        return false;
      }
      if (not hdr_writer.append((hdr.sn & 0x000000ffU))) {
        return false;
      }
      break;
    default:
      logger.log_error("Invalid sn_size={}", cfg.sn_size);
      return false;
  }
  return true;
}

/*
 * Timers
 */
void pdcp_entity_tx::stop_discard_timer(uint32_t highest_count)
{
  if (!cfg.discard_timer.has_value()) {
    logger.log_debug("Cannot stop discard timers. No discard timer configured. highest_count={}", highest_count);
    return;
  }
  if (highest_count < st.tx_next_ack || highest_count >= st.tx_next) {
    logger.log_warning("Cannot stop discard timers. highest_count={} is outside tx_window. {}", highest_count, st);
    return;
  }
  logger.log_debug("Stopping discard timers. highest_count={}", highest_count);

  // Stop discard timers and update TX_NEXT_ACK to oldest element in tx_window
  while (st.tx_next_ack <= highest_count) {
    if (tx_window->has_sn(st.tx_next_ack)) {
      tx_window->remove_sn(st.tx_next_ack);
      logger.log_debug("Stopped discard timer. count={}", st.tx_next_ack);
    }
    st.tx_next_ack++;
  }

  // Update TX_TRANS if it falls out of the tx_window
  if (st.tx_trans < st.tx_next_ack) {
    st.tx_trans = st.tx_next_ack;
  }
}

void pdcp_entity_tx::discard_pdu(uint32_t count)
{
  if (!cfg.discard_timer.has_value()) {
    logger.log_debug("Cannot discard PDU. No discard timer configured. count={}", count);
    return;
  }
  if (count < st.tx_next_ack || count >= st.tx_next) {
    logger.log_warning("Cannot discard PDU. The PDU is outside tx_window. count={} {}", count, st);
    return;
  }
  if (!tx_window->has_sn(count)) {
    logger.log_warning("Cannot discard PDU. The PDU is missing in tx_window. count={} {}", count, st);
    return;
  }
  logger.log_debug("Discarding PDU. count={}", count);

  // Notify lower layers of the discard. It's the RLC to actually discard, if no segment was transmitted yet.
  lower_dn.on_discard_pdu(SN(count));

  tx_window->remove_sn(count);

  // Update TX_NEXT_ACK to oldest element in tx_window
  while (st.tx_next_ack < st.tx_next && !tx_window->has_sn(st.tx_next_ack)) {
    st.tx_next_ack++;
  }

  // Update TX_TRANS if it falls out of the tx_window
  if (st.tx_trans < st.tx_next_ack) {
    st.tx_trans = st.tx_next_ack;
  }
}

std::unique_ptr<sdu_window<pdcp_entity_tx::pdcp_tx_sdu_info>> pdcp_entity_tx::create_tx_window(pdcp_sn_size sn_size_)
{
  std::unique_ptr<sdu_window<pdcp_tx_sdu_info>> tx_window_;
  switch (sn_size_) {
    case pdcp_sn_size::size12bits:
      tx_window_ = std::make_unique<sdu_window_impl<pdcp_tx_sdu_info,
                                                    pdcp_window_size(pdcp_sn_size_to_uint(pdcp_sn_size::size12bits)),
                                                    pdcp_bearer_logger>>(logger);
      break;
    case pdcp_sn_size::size18bits:
      tx_window_ = std::make_unique<sdu_window_impl<pdcp_tx_sdu_info,
                                                    pdcp_window_size(pdcp_sn_size_to_uint(pdcp_sn_size::size18bits)),
                                                    pdcp_bearer_logger>>(logger);
      break;
    default:
      srsran_assertion_failure("Cannot create tx_window for unsupported sn_size={}.", pdcp_sn_size_to_uint(sn_size_));
  }
  return tx_window_;
}

// Discard Timer Callback (discardTimer)
void pdcp_entity_tx::discard_callback::operator()(timer_id_t timer_id)
{
  parent->logger.log_debug("Discard timer expired. count={}", discard_count);

  // Add discard to metrics
  parent->metrics_add_discard_timouts(1);

  // Discard PDU
  // NOTE: this will delete the callback. It *must* be the last instruction.
  parent->discard_pdu(discard_count);
}
