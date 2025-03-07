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

#include "ue_event_manager.h"
#include "../config/sched_config_manager.h"
#include "../logging/scheduler_event_logger.h"
#include "../logging/scheduler_metrics_handler.h"

using namespace srsran;

/// \brief More than one DL buffer occupancy update may be received per slot for the same UE and bearer. This class
/// ensures that the UE DL buffer occupancy is updated only once per bearer per slot for efficiency reasons.
class ue_event_manager::ue_dl_buffer_occupancy_manager final : public scheduler_dl_buffer_state_indication_handler
{
  using bearer_key                        = uint32_t;
  static constexpr size_t NOF_BEARER_KEYS = MAX_NOF_DU_UES * MAX_NOF_RB_LCIDS;

  static bearer_key    get_bearer_key(du_ue_index_t ue_index, lcid_t lcid) { return lcid * MAX_NOF_DU_UES + ue_index; }
  static du_ue_index_t get_ue_index(bearer_key key) { return to_du_ue_index(key % MAX_NOF_DU_UES); }
  static lcid_t        get_lcid(bearer_key key) { return uint_to_lcid(key / MAX_NOF_DU_UES); }

public:
  ue_dl_buffer_occupancy_manager(ue_event_manager& parent_) : parent(parent_)
  {
    std::fill(ue_dl_bo_table.begin(), ue_dl_bo_table.end(), -1);
  }

  void handle_dl_buffer_state_indication(const dl_buffer_state_indication_message& rlc_dl_bo) override
  {
    // Update DL Buffer Occupancy for the given UE and bearer.
    unsigned key          = get_bearer_key(rlc_dl_bo.ue_index, rlc_dl_bo.lcid);
    bool     first_rlc_bo = ue_dl_bo_table[key].exchange(rlc_dl_bo.bs, std::memory_order_acquire) < 0;

    if (not first_rlc_bo) {
      // If another DL BO update has been received before for this same bearer, we do not need to enqueue a new event.
      return;
    }

    // Signal that this bearer needs its BO state updated.
    pending_evs.push(key);
  }

  void slot_indication()
  {
    // Retrieve pending UEs.
    pending_evs.slot_indication();
    span<bearer_key> ues_to_process = pending_evs.get_events();

    // Process RLC buffer updates of pending UEs.
    for (bearer_key key : ues_to_process) {
      // Recreate latest DL BO update.
      dl_buffer_state_indication_message dl_bo;
      // > Extract UE index and LCID.
      dl_bo.ue_index = get_ue_index(key);
      dl_bo.lcid     = get_lcid(key);
      // > Extract last DL BO value for the respective bearer and reset BO table position.
      dl_bo.bs = ue_dl_bo_table[key].exchange(-1, std::memory_order_acq_rel);
      if (dl_bo.bs < 0) {
        parent.logger.warning(
            "ue={} lcid={}: Invalid DL buffer occupancy value: {}", dl_bo.ue_index, dl_bo.lcid, dl_bo.bs);
        continue;
      }

      // Retrieve UE.
      if (not parent.ue_db.contains(dl_bo.ue_index)) {
        parent.log_invalid_ue_index(dl_bo.ue_index);
        continue;
      }
      ue& u = parent.ue_db[dl_bo.ue_index];

      // Forward DL BO update to UE.
      u.handle_dl_buffer_state_indication(dl_bo);
      if (dl_bo.lcid == LCID_SRB0) {
        // Signal SRB0 scheduler with the new SRB0 buffer state.
        parent.du_cells[u.get_pcell().cell_index].srb0_sched->handle_dl_buffer_state_indication(dl_bo.ue_index);
      }

      // Log event.
      parent.ev_logger.enqueue(dl_bo);

      // Report event.
      parent.metrics_handler.handle_dl_buffer_state_indication(dl_bo);
    }
  }

private:
  ue_event_manager& parent;

  // Table of pending DL Buffer Occupancy values. -1 means that no DL Buffer Occupancy is set.
  std::array<std::atomic<int>, NOF_BEARER_KEYS> ue_dl_bo_table;

  slot_event_list<bearer_key> pending_evs;
};

ue_event_manager::ue_event_manager(ue_repository&             ue_db_,
                                   scheduler_metrics_handler& metrics_handler_,
                                   scheduler_event_logger&    ev_logger_) :
  ue_db(ue_db_),
  metrics_handler(metrics_handler_),
  ev_logger(ev_logger_),
  logger(srslog::fetch_basic_logger("SCHED")),
  dl_bo_mng(std::make_unique<ue_dl_buffer_occupancy_manager>(*this))
{
}

ue_event_manager::~ue_event_manager() {}

void ue_event_manager::handle_ue_creation(ue_config_update_event ev)
{
  // Create UE object outside the scheduler slot indication handler to minimize latency.
  std::unique_ptr<ue> u = std::make_unique<ue>(ue_creation_command{
      ev.next_config(), ev.get_fallback_command().has_value() and ev.get_fallback_command().value(), metrics_handler});

  // Defer UE object addition to ue list to the slot indication handler.
  common_events.emplace(INVALID_DU_UE_INDEX, [this, u = std::move(u), ev = std::move(ev)]() mutable {
    if (ue_db.contains(u->ue_index)) {
      logger.error("ue={} rnti={}: Discarding UE creation. Cause: A UE with the same index already exists",
                   u->ue_index,
                   u->crnti);
      ev.abort();
      return;
    }

    // Insert UE in UE repository.
    du_ue_index_t   ueidx       = u->ue_index;
    rnti_t          rnti        = u->crnti;
    du_cell_index_t pcell_index = u->get_pcell().cell_index;
    ue_db.add_ue(std::move(u));

    // Log Event.
    ev_logger.enqueue(scheduler_event_logger::ue_creation_event{ueidx, rnti, pcell_index});
  });
}

void ue_event_manager::handle_ue_reconfiguration(ue_config_update_event ev)
{
  du_ue_index_t ue_index = ev.get_ue_index();
  common_events.emplace(ue_index, [this, ev = std::move(ev)]() mutable {
    const du_ue_index_t ue_idx = ev.get_ue_index();
    if (not ue_db.contains(ue_idx)) {
      log_invalid_ue_index(ue_idx, "UE Reconfig Request");
      ev.abort();
      return;
    }

    // Configure existing UE.
    ue_db[ue_idx].handle_reconfiguration_request(ue_reconf_command{ev.next_config()});

    // Log event.
    ev_logger.enqueue(scheduler_event_logger::ue_reconf_event{ue_idx, ue_db[ue_idx].crnti});
  });
}

void ue_event_manager::handle_ue_deletion(ue_config_delete_event ev)
{
  const du_ue_index_t ue_index = ev.ue_index();
  common_events.emplace(ue_index, [this, ev = std::move(ev)]() mutable {
    const du_ue_index_t ue_idx = ev.ue_index();
    if (not ue_db.contains(ue_idx)) {
      logger.warning("Received request to delete ue={} that does not exist", ue_idx);
      return;
    }
    const rnti_t rnti = ue_db[ue_idx].crnti;

    // Scheduler UE removal from repository.
    ue_db.schedule_ue_rem(std::move(ev));

    // Log UE removal event.
    ev_logger.enqueue(sched_ue_delete_message{ue_idx, rnti});
  });
}

void ue_event_manager::handle_ul_bsr_indication(const ul_bsr_indication_message& bsr_ind)
{
  srsran_sanity_check(cell_exists(bsr_ind.cell_index), "Invalid cell index");

  common_events.emplace(bsr_ind.ue_index, [this, bsr_ind]() {
    if (not ue_db.contains(bsr_ind.ue_index)) {
      log_invalid_ue_index(bsr_ind.ue_index, "BSR");
      return;
    }
    auto& u = ue_db[bsr_ind.ue_index];
    // Handle event.
    u.handle_bsr_indication(bsr_ind);

    // Log event.
    if (ev_logger.enabled()) {
      scheduler_event_logger::bsr_event event{};
      event.ue_index             = bsr_ind.ue_index;
      event.rnti                 = bsr_ind.crnti;
      event.type                 = bsr_ind.type;
      event.reported_lcgs        = bsr_ind.reported_lcgs;
      event.tot_ul_pending_bytes = units::bytes{u.pending_ul_newtx_bytes()};
      ev_logger.enqueue(event);
    }

    // Notify metrics handler.
    metrics_handler.handle_ul_bsr_indication(bsr_ind);
  });
}

void ue_event_manager::handle_ul_phr_indication(const ul_phr_indication_message& phr_ind)
{
  for (const cell_ph_report& cell_phr : phr_ind.phr.get_phr()) {
    srsran_sanity_check(cell_exists(cell_phr.serv_cell_id), "Invalid serving cell index={}", cell_phr.serv_cell_id);

    cell_specific_events[cell_phr.serv_cell_id].emplace(
        phr_ind.ue_index,
        [this, cell_phr, phr_ind](ue_cell& ue_cc) {
          ue_cc.channel_state_manager().handle_phr(cell_phr);

          // Log event.
          scheduler_event_logger::phr_event event{};
          event.ue_index   = phr_ind.ue_index;
          event.rnti       = phr_ind.rnti;
          event.cell_index = cell_phr.serv_cell_id;
          event.ph         = cell_phr.ph;
          event.p_cmax     = cell_phr.p_cmax;
          ev_logger.enqueue(event);

          // Notify metrics handler.
          metrics_handler.handle_ul_phr_indication(phr_ind);
        },
        "UL PHR",
        true);
  }
}

void ue_event_manager::handle_crc_indication(const ul_crc_indication& crc_ind)
{
  srsran_assert(cell_exists(crc_ind.cell_index), "Invalid cell index");

  for (unsigned i = 0; i != crc_ind.crcs.size(); ++i) {
    cell_specific_events[crc_ind.cell_index].emplace(
        crc_ind.crcs[i].ue_index,
        [this, sl_rx = crc_ind.sl_rx, crc = crc_ind.crcs[i]](ue_cell& ue_cc) {
          const int tbs = ue_cc.handle_crc_pdu(sl_rx, crc);
          if (tbs < 0) {
            return;
          }

          // Log event.
          ev_logger.enqueue(scheduler_event_logger::crc_event{
              crc.ue_index, crc.rnti, ue_cc.cell_index, sl_rx, crc.harq_id, crc.tb_crc_success, crc.ul_sinr_metric});

          // Notify metrics handler.
          metrics_handler.handle_crc_indication(crc, units::bytes{(unsigned)tbs});

          // Process Timing Advance Offset.
          if (crc.tb_crc_success and crc.time_advance_offset.has_value() and crc.ul_sinr_metric.has_value()) {
            ue_db[ue_cc.ue_index].handle_ul_n_ta_update_indication(
                ue_cc.cell_index, crc.ul_sinr_metric.value(), crc.time_advance_offset.value());
          }
        },
        "CRC",
        true);
  }
}

void ue_event_manager::handle_harq_ind(ue_cell&                               ue_cc,
                                       slot_point                             uci_sl,
                                       span<const mac_harq_ack_report_status> harq_bits,
                                       optional<float>                        pucch_snr)
{
  for (unsigned harq_idx = 0; harq_idx != harq_bits.size(); ++harq_idx) {
    // Update UE HARQ state with received HARQ-ACK.
    dl_harq_process::dl_ack_info_result result =
        ue_cc.handle_dl_ack_info(uci_sl, harq_bits[harq_idx], harq_idx, pucch_snr);

    if (result.h_id != INVALID_HARQ_ID) {
      // Respective HARQ was found.
      const units::bytes tbs{result.tbs_bytes};

      // Log Event.
      ev_logger.enqueue(scheduler_event_logger::harq_ack_event{
          ue_cc.ue_index, ue_cc.rnti(), ue_cc.cell_index, uci_sl, result.h_id, harq_bits[harq_idx], tbs});

      if (result.update == dl_harq_process::status_update::acked or
          result.update == dl_harq_process::status_update::nacked) {
        // In case the HARQ process is not waiting for more HARQ-ACK bits. Notify metrics handler with HARQ outcome.
        metrics_handler.handle_dl_harq_ack(ue_cc.ue_index, result.update == dl_harq_process::status_update::acked, tbs);
      }
    }
  }
}

void ue_event_manager::handle_csi(ue_cell& ue_cc, const csi_report_data& csi_rep)
{
  // Forward CSI bits to UE.
  ue_cc.handle_csi_report(csi_rep);

  // Log event.
  ev_logger.enqueue(scheduler_event_logger::csi_report_event{ue_cc.ue_index, ue_cc.rnti(), csi_rep});

  // Report the CSI metric.
  metrics_handler.handle_csi_report(ue_cc.ue_index, csi_rep);
}

void ue_event_manager::handle_uci_indication(const uci_indication& ind)
{
  srsran_sanity_check(cell_exists(ind.cell_index), "Invalid cell index");

  for (unsigned i = 0; i != ind.ucis.size(); ++i) {
    const uci_indication::uci_pdu& uci = ind.ucis[i];

    cell_specific_events[ind.cell_index].emplace(
        uci.ue_index,
        [this, uci_sl = ind.slot_rx, uci_pdu = uci.pdu](ue_cell& ue_cc) {
          if (variant_holds_alternative<uci_indication::uci_pdu::uci_pucch_f0_or_f1_pdu>(uci_pdu)) {
            const auto& pdu = variant_get<uci_indication::uci_pdu::uci_pucch_f0_or_f1_pdu>(uci_pdu);

            // Process DL HARQ ACKs.
            if (not pdu.harqs.empty()) {
              handle_harq_ind(ue_cc, uci_sl, pdu.harqs, pdu.ul_sinr);
            }

            // Process SRs.
            if (pdu.sr_detected) {
              // Handle SR indication.
              ue_db[ue_cc.ue_index].handle_sr_indication();

              // Log SR event.
              ev_logger.enqueue(scheduler_event_logger::sr_event{ue_cc.ue_index, ue_cc.rnti()});
            }

            // Report the PUCCH SINR metric.
            metrics_handler.handle_pucch_sinr(ue_cc.ue_index, pdu.ul_sinr);

            const bool is_uci_valid = not pdu.harqs.empty() or pdu.sr_detected;
            // Process Timing Advance Offset.
            if (is_uci_valid and pdu.time_advance_offset.has_value() and pdu.ul_sinr.has_value()) {
              ue_db[ue_cc.ue_index].handle_ul_n_ta_update_indication(
                  ue_cc.cell_index, pdu.ul_sinr.value(), pdu.time_advance_offset.value());
            }

          } else if (variant_holds_alternative<uci_indication::uci_pdu::uci_pusch_pdu>(uci_pdu)) {
            const auto& pdu = variant_get<uci_indication::uci_pdu::uci_pusch_pdu>(uci_pdu);

            // Process DL HARQ ACKs.
            if (not pdu.harqs.empty()) {
              handle_harq_ind(ue_cc, uci_sl, pdu.harqs, nullopt);
            }

            // Process CSI.
            if (pdu.csi.has_value()) {
              handle_csi(ue_cc, *pdu.csi);
            }

          } else if (variant_holds_alternative<uci_indication::uci_pdu::uci_pucch_f2_or_f3_or_f4_pdu>(uci_pdu)) {
            const auto& pdu = variant_get<uci_indication::uci_pdu::uci_pucch_f2_or_f3_or_f4_pdu>(uci_pdu);

            // Process DL HARQ ACKs.
            if (not pdu.harqs.empty()) {
              handle_harq_ind(ue_cc, uci_sl, pdu.harqs, pdu.ul_sinr);
            }

            // Process SRs.
            const size_t sr_bit_position_with_1_sr_bit = 0;
            if (not pdu.sr_info.empty() and pdu.sr_info.test(sr_bit_position_with_1_sr_bit)) {
              // Handle SR indication.
              ue_db[ue_cc.ue_index].handle_sr_indication();

              // Log SR event.
              ev_logger.enqueue(scheduler_event_logger::sr_event{ue_cc.ue_index, ue_cc.rnti()});
            }

            // Process CSI.
            if (pdu.csi.has_value()) {
              handle_csi(ue_cc, *pdu.csi);
            }

            // Report the PUCCH metric to the scheduler.
            metrics_handler.handle_pucch_sinr(ue_cc.ue_index, pdu.ul_sinr);

            const bool is_uci_valid = not pdu.harqs.empty() or
                                      (not pdu.sr_info.empty() and pdu.sr_info.test(sr_bit_position_with_1_sr_bit)) or
                                      pdu.csi.has_value();
            // Process Timing Advance Offset.
            if (is_uci_valid and pdu.time_advance_offset.has_value() and pdu.ul_sinr.has_value()) {
              ue_db[ue_cc.ue_index].handle_ul_n_ta_update_indication(
                  ue_cc.cell_index, pdu.ul_sinr.value(), pdu.time_advance_offset.value());
            }
          }
        },
        "UCI",
        // Note: We do not warn if the UE is not found, because there is this transient period when the UE
        // is about to receive and process the RRC Release, but it is still sending CSI or SR in the PUCCH. If we stop
        // the PUCCH scheduling for the UE about to be released, we could risk interference between UEs in the PUCCH.
        false);
  }
}

void ue_event_manager::handle_dl_mac_ce_indication(const dl_mac_ce_indication& ce)
{
  common_events.emplace(ce.ue_index, [this, ce]() {
    if (not ue_db.contains(ce.ue_index)) {
      log_invalid_ue_index(ce.ue_index, "DL MAC CE");
      return;
    }
    ue_db[ce.ue_index].handle_dl_mac_ce_indication(ce);

    // Log event.
    ev_logger.enqueue(ce);
  });
}

void ue_event_manager::handle_dl_buffer_state_indication(const dl_buffer_state_indication_message& bs)
{
  dl_bo_mng->handle_dl_buffer_state_indication(bs);
}

void ue_event_manager::process_common(slot_point sl, du_cell_index_t cell_index)
{
  bool new_slot_detected = last_sl != sl;
  if (new_slot_detected) {
    // Pop pending common events.
    common_events.slot_indication();
    last_sl = sl;
  }

  // Process events for UEs whose PCell matches cell_index argument.
  span<common_event_t> events_to_process = common_events.get_events();
  for (common_event_t& ev : events_to_process) {
    if (ev.callback.is_empty()) {
      // Event already processed.
      continue;
    }
    if (ev.ue_index == MAX_NOF_DU_UES) {
      // The UE is being created.
      ev.callback();
      ev.callback = {};
    } else {
      if (not ue_db.contains(ev.ue_index)) {
        // Can't find UE. Log error.
        log_invalid_ue_index(ev.ue_index);
        ev.callback = {};
        continue;
      }
      if (ue_db[ev.ue_index].get_pcell().cell_index == cell_index) {
        // If we are currently processing PCell.
        ev.callback();
        ev.callback = {};
      }
    }
  }

  if (new_slot_detected) {
    dl_bo_mng->slot_indication();
  }
}

void ue_event_manager::process_cell_specific(du_cell_index_t cell_index)
{
  // Pop and process pending cell-specific events.
  cell_specific_events[cell_index].slot_indication();
  auto events = cell_specific_events[cell_index].get_events();
  for (cell_event_t& ev : events) {
    if (not ue_db.contains(ev.ue_index)) {
      log_invalid_ue_index(ev.ue_index, ev.event_name, ev.warn_if_ignored);
      continue;
    }
    ue&      ue    = ue_db[ev.ue_index];
    ue_cell* ue_cc = ue.find_cell(cell_index);
    if (ue_cc == nullptr) {
      log_invalid_cc(ev.ue_index, cell_index);
      continue;
    }
    ev.callback(*ue_cc);
  }
}

void ue_event_manager::run(slot_point sl, du_cell_index_t cell_index)
{
  srsran_sanity_check(cell_exists(cell_index), "Invalid cell index {}", cell_index);

  // Process common events.
  process_common(sl, cell_index);

  // Process carrier specific events.
  process_cell_specific(cell_index);
}

void ue_event_manager::add_cell(const cell_configuration& cell_cfg_, ue_srb0_scheduler& srb0_sched)
{
  srsran_assert(not cell_exists(cell_cfg_.cell_index), "Overwriting cell configurations not supported");

  du_cells[cell_cfg_.cell_index].cfg        = &cell_cfg_;
  du_cells[cell_cfg_.cell_index].srb0_sched = &srb0_sched;
}

bool ue_event_manager::cell_exists(du_cell_index_t cell_index) const
{
  return cell_index < MAX_NOF_DU_CELLS and du_cells[cell_index].cfg != nullptr;
}

void ue_event_manager::log_invalid_ue_index(du_ue_index_t ue_index, const char* event_name, bool warn_if_ignored) const
{
  srslog::log_channel& log_channel = warn_if_ignored ? logger.warning : logger.info;
  log_channel("{} for ue={} discarded. Cause: UE with provided Id does not exist", event_name, ue_index);
}

void ue_event_manager::log_invalid_cc(du_ue_index_t ue_index, du_cell_index_t cell_index) const
{
  logger.warning("Event for ue={} ignored. Cause: Cell {} is not configured.", ue_index, cell_index);
}
