/*
 *
 * Copyright 2013-2022 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "pdcch_scheduler.h"
#include "../phy_helpers.h"

using namespace srsgnb;

class pdcch_scheduler::pdcch_slot_allocator
{
public:
  /// PDCCH grant allocation in a given slot.
  struct alloc_record {
    bool                              is_dl;
    pdcch_information*                pdcch;
    const search_space_configuration* ss_cfg;
  };

  /// DFS decision tree node.
  struct tree_node {
    unsigned   dci_iter_index;
    unsigned   ncce;
    unsigned   record_index;
    grant_info grant;
  };

  explicit pdcch_slot_allocator(const cell_configuration& cell_cfg_, unsigned slot_index);
  ~pdcch_slot_allocator() {}

  void clear();

  bool alloc_pdcch(pdcch_information&                pdcch,
                   cell_slot_resource_allocator&     slot_alloc,
                   const search_space_configuration& ss_cfg);

private:
  bool alloc_dfs_node(cell_slot_resource_allocator& slot_alloc, const alloc_record& record, unsigned dci_iter_index);
  bool get_next_dfs(cell_slot_resource_allocator& slot_alloc);

  span<const unsigned> get_cce_loc_table(const alloc_record& record) const;

  const cell_configuration& cell_cfg;
  unsigned                  slot_index;

  /// list of grants in a given slot.
  static_vector<alloc_record, MAX_GRANTS> records;

  /// dfs decision tree for the given slot.
  std::vector<tree_node> dfs_tree, saved_dfs_tree;
};

pdcch_scheduler::pdcch_slot_allocator::pdcch_slot_allocator(const cell_configuration& cell_cfg_, unsigned slot_index_) :
  cell_cfg(cell_cfg_), slot_index(slot_index_)
{}

void pdcch_scheduler::pdcch_slot_allocator::clear()
{
  records.clear();
  dfs_tree.clear();
  saved_dfs_tree.clear();
}

bool pdcch_scheduler::pdcch_slot_allocator::alloc_pdcch(pdcch_information&                pdcch,
                                                        cell_slot_resource_allocator&     slot_alloc,
                                                        const search_space_configuration& ss_cfg)
{
  saved_dfs_tree.clear();

  // Create an DL Allocation Record.
  alloc_record record{};
  record.is_dl  = true;
  record.pdcch  = &pdcch;
  record.ss_cfg = &ss_cfg;

  // Try to allocate PDCCH for one of the possible CCE positions. If this operation fails, retry it, but using a
  // different permutation of past grant CCE positions.
  do {
    if (alloc_dfs_node(slot_alloc, record, 0)) {
      // DCI record was successfully allocated.
      records.push_back(record);
      return true;
    }
    if (saved_dfs_tree.empty()) {
      saved_dfs_tree = dfs_tree;
    }
  } while (get_next_dfs(slot_alloc));

  // Revert steps to initial state, before dci record allocation was attempted
  dfs_tree.swap(saved_dfs_tree);
  return false;
}

bool pdcch_scheduler::pdcch_slot_allocator::alloc_dfs_node(cell_slot_resource_allocator& slot_alloc,
                                                           const alloc_record&           record,
                                                           unsigned                      dci_iter_index)
{
  // Get CCE location table, i.e. the current node possible leaves.
  auto cce_locs = get_cce_loc_table(record);
  if (dci_iter_index >= cce_locs.size()) {
    // All possible CCE position leaves have been attempted. Early return.
    return false;
  }

  // Create new tree leave.
  tree_node node{};
  node.dci_iter_index = dci_iter_index;
  node.record_index   = dfs_tree.size();
  node.grant.ch       = grant_info::channel::cch;
  node.grant.scs      = record.pdcch->bwp_cfg->scs;

  // Find in the list of possible CCEs, a CCE that does not cause PDCCH collisions.
  for (; node.dci_iter_index < cce_locs.size(); ++node.dci_iter_index) {
    node.ncce = cce_locs[node.dci_iter_index];

    // Check the current CCE position collides with an existing one.
    // TODO: Optimize.
    pdcch_reg_position_list regs =
        get_pdcch_regs(*record.pdcch->coreset_cfg, *record.ss_cfg, node.ncce, record.pdcch->dci.aggr_level);
    for (pdcch_reg_position& reg : regs) {
      unsigned crb       = prb_to_crb(*record.pdcch->bwp_cfg, reg.prb);
      node.grant.crbs    = {crb, crb + 1};
      node.grant.symbols = {reg.symbol, (uint8_t)(reg.symbol + 1)};
      if (slot_alloc.dl_res_grid.collides(node.grant)) {
        // Collision detected. Try another CCE position.
        continue;
      }
    }

    // Allocation successful.
    // TODO: Optimize.
    for (pdcch_reg_position& reg : regs) {
      unsigned crb       = prb_to_crb(*record.pdcch->bwp_cfg, reg.prb);
      node.grant.crbs    = {crb, crb + 1};
      node.grant.symbols = {reg.symbol, (uint8_t)(reg.symbol + 1)};
      slot_alloc.dl_res_grid.fill(node.grant);
    }
    dfs_tree.push_back(node);
    return true;
  }
  return false;
}

bool pdcch_scheduler::pdcch_slot_allocator::get_next_dfs(cell_slot_resource_allocator& slot_alloc)
{
  do {
    if (dfs_tree.empty()) {
      // If we reach root, the allocation failed.
      return false;
    }
    // Attempt to re-add last tree node, but with a higher leaf index.
    uint32_t start_child_idx = dfs_tree.back().dci_iter_index + 1;
    dfs_tree.pop_back();
    while (dfs_tree.size() < records.size() and alloc_dfs_node(slot_alloc, records[dfs_tree.size()], start_child_idx)) {
      start_child_idx = 0;
    }
  } while (dfs_tree.size() < records.size());

  // Finished computation of next DFS node.
  return true;
}

span<const unsigned> pdcch_scheduler::pdcch_slot_allocator::get_cce_loc_table(const alloc_record& record) const
{
  // TODO
  static const static_vector<unsigned, 1> cces = {0};
  return cces;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

pdcch_scheduler::pdcch_scheduler(cell_resource_allocator& res_grid_) : res_alloc(res_grid_)
{
  subcarrier_spacing max_scs             = res_alloc.cfg.dl_cfg_common.freq_info_dl.scs_carrier_list.back().scs;
  unsigned           nof_slots_per_frame = NOF_SUBFRAMES_PER_FRAME * get_nof_slots_per_subframe(max_scs);
  for (unsigned i = 0; i < SLOT_ALLOCATOR_RING_SIZE; ++i) {
    unsigned slot_index = i % nof_slots_per_frame;
    slot_records[i]     = std::make_unique<pdcch_slot_allocator>(res_alloc.cfg, slot_index);
  }
}

pdcch_scheduler::~pdcch_scheduler() = default;

void pdcch_scheduler::slot_indication(slot_point sl_tx)
{
  srsran_sanity_check(not last_sl_ind.valid() or sl_tx == last_sl_ind + 1, "Detected skipped slot");

  // Update Slot.
  last_sl_ind = sl_tx;

  // Clear old records.
  slot_records[(last_sl_ind - 1).to_uint() % slot_records.size()]->clear();
}

pdcch_information*
pdcch_scheduler::alloc_pdcch_common(slot_point sl_tx, rnti_t rnti, search_space_id ss_id, aggregation_level aggr_lvl)
{
  // Find Common BWP and CORESET configurations.
  const bwp_configuration&          bwp_cfg = res_alloc.cfg.dl_cfg_common.init_dl_bwp.generic_params;
  const search_space_configuration& ss_cfg =
      res_alloc.cfg.dl_cfg_common.init_dl_bwp.pdcch_common.search_spaces[(size_t)ss_id];
  const coreset_configuration& cs_cfg = res_alloc.cfg.dl_cfg_common.init_dl_bwp.pdcch_common.coresets[ss_cfg.cs_id];

  return alloc_dl_pdcch_helper(sl_tx, rnti, bwp_cfg, cs_cfg, ss_cfg, aggr_lvl, dci_dl_format::f1_0);
}

pdcch_information* pdcch_scheduler::alloc_pdcch_ue(slot_point                      sl_tx,
                                                   rnti_t                          rnti,
                                                   const ue_carrier_configuration& user,
                                                   du_bwp_id_t                     bwp_id,
                                                   search_space_id                 ss_id,
                                                   aggregation_level               aggr_lvl,
                                                   dci_dl_format                   dci_fmt)
{
  // Find Common or UE-specific BWP and CORESET configurations.
  const bwp_configuration&          bwp_cfg = *user.get_bwp_cfg(bwp_id);
  const search_space_configuration& ss_cfg  = *user.get_ss_cfg(bwp_id, ss_id);
  const coreset_configuration&      cs_cfg  = *user.get_cs_cfg(bwp_id, ss_cfg.cs_id);

  return alloc_dl_pdcch_helper(sl_tx, rnti, bwp_cfg, cs_cfg, ss_cfg, aggr_lvl, dci_fmt);
}

pdcch_information* pdcch_scheduler::alloc_dl_pdcch_helper(slot_point                        sl_tx,
                                                          rnti_t                            rnti,
                                                          const bwp_configuration&          bwp_cfg,
                                                          const coreset_configuration&      cs_cfg,
                                                          const search_space_configuration& ss_cfg,
                                                          aggregation_level                 L,
                                                          dci_dl_format                     dci_fmt)
{
  pdcch_slot_allocator&         pdcch_alloc = get_slot_alloc(sl_tx);
  cell_slot_resource_allocator& slot_alloc  = res_alloc[sl_tx];

  // Create PDCCH list element.
  slot_alloc.result.dl.pdcchs.emplace_back();
  pdcch_information& pdcch = slot_alloc.result.dl.pdcchs.back();
  pdcch.bwp_cfg            = &bwp_cfg;
  pdcch.coreset_cfg        = &cs_cfg;
  pdcch.dci.rnti           = rnti;
  pdcch.dci.aggr_level     = L;
  pdcch.dci.format_type    = dci_fmt;

  // Allocate a position for DL PDCCH in CORESET.
  if (not pdcch_alloc.alloc_pdcch(pdcch, slot_alloc, ss_cfg)) {
    slot_alloc.result.dl.pdcchs.pop_back();
    return nullptr;
  }
  return &pdcch;
}

pdcch_scheduler::pdcch_slot_allocator& pdcch_scheduler::get_slot_alloc(slot_point sl)
{
  srsran_sanity_check(sl < last_sl_ind + SLOT_ALLOCATOR_RING_SIZE, "PDCCH being allocated to far into the future");
  return *slot_records[sl.to_uint() % slot_records.size()];
}
