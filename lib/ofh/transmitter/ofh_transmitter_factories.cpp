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

#include "ofh_transmitter_factories.h"
#include "ofh_data_flow_cplane_scheduling_commands_task_dispatcher.h"
#include "ofh_data_flow_uplane_downlink_task_dispatcher.h"
#include "ofh_downlink_manager_broadcast_impl.h"
#include "ofh_downlink_manager_impl.h"
#include "ofh_transmitter_impl.h"
#include "ofh_uplane_fragment_size_calculator.h"
#include "ofh_uplink_request_handler_task_dispatcher.h"
#include "srsran/ofh/compression/compression_factory.h"
#include "srsran/ofh/ecpri/ecpri_factories.h"
#include "srsran/ofh/ethernet/ethernet_factories.h"
#include "srsran/ofh/serdes/ofh_serdes_factories.h"

#ifdef DPDK_FOUND
#include "../ethernet/dpdk/dpdk_ethernet_factories.h"
#endif

using namespace srsran;
using namespace ofh;

static std::unique_ptr<data_flow_cplane_scheduling_commands>
create_data_flow_cplane_sched(const transmitter_config&                         tx_config,
                              srslog::basic_logger&                             logger,
                              std::shared_ptr<ether::eth_frame_pool>            frame_pool,
                              std::shared_ptr<uplink_cplane_context_repository> ul_cplane_context_repo)
{
  data_flow_cplane_scheduling_commands_impl_config config;

  config.ru_nof_prbs =
      get_max_Nprb(bs_channel_bandwidth_to_MHz(tx_config.ru_working_bw), tx_config.scs, srsran::frequency_range::FR1);
  config.vlan_params.eth_type        = ether::ECPRI_ETH_TYPE;
  config.vlan_params.tci             = tx_config.tci;
  config.vlan_params.mac_dst_address = tx_config.mac_dst_address;
  config.vlan_params.mac_src_address = tx_config.mac_src_address;
  config.dl_compr_params             = tx_config.dl_compr_params;
  config.ul_compr_params             = tx_config.ul_compr_params;
  config.prach_compr_params          = tx_config.prach_compr_params;

  data_flow_cplane_scheduling_commands_impl_dependencies dependencies;
  dependencies.logger                 = &logger;
  dependencies.ul_cplane_context_repo = ul_cplane_context_repo;
  dependencies.frame_pool             = frame_pool;
  dependencies.eth_builder            = ether::create_vlan_frame_builder();
  dependencies.ecpri_builder          = ecpri::create_ecpri_packet_builder();
  dependencies.cp_builder             = (tx_config.is_downlink_static_compr_hdr_enabled)
                                            ? ofh::create_ofh_control_plane_static_compression_message_builder()
                                            : ofh::create_ofh_control_plane_dynamic_compression_message_builder();

  return std::make_unique<data_flow_cplane_scheduling_commands_impl>(config, std::move(dependencies));
}

static std::unique_ptr<data_flow_uplane_downlink_data>
create_data_flow_uplane_data(const transmitter_config&              tx_config,
                             srslog::basic_logger&                  logger,
                             std::shared_ptr<ether::eth_frame_pool> frame_pool)
{
  data_flow_uplane_downlink_data_impl_config config;
  config.ru_nof_prbs =
      get_max_Nprb(bs_channel_bandwidth_to_MHz(tx_config.ru_working_bw), tx_config.scs, srsran::frequency_range::FR1);
  config.vlan_params.eth_type        = ether::ECPRI_ETH_TYPE;
  config.vlan_params.tci             = tx_config.tci;
  config.vlan_params.mac_dst_address = tx_config.mac_dst_address;
  config.vlan_params.mac_src_address = tx_config.mac_src_address;
  config.compr_params                = tx_config.dl_compr_params;

  data_flow_uplane_downlink_data_impl_dependencies dependencies;
  dependencies.logger        = &logger;
  dependencies.frame_pool    = frame_pool;
  dependencies.eth_builder   = ether::create_vlan_frame_builder();
  dependencies.ecpri_builder = ecpri::create_ecpri_packet_builder();

  const unsigned nof_prbs =
      get_max_Nprb(bs_channel_bandwidth_to_MHz(tx_config.bw), tx_config.scs, srsran::frequency_range::FR1);
  const double bw_scaling = 1.0 / (std::sqrt(nof_prbs * NOF_SUBCARRIERS_PER_RB));

  std::array<std::unique_ptr<ofh::iq_compressor>, ofh::NOF_COMPRESSION_TYPES_SUPPORTED> compressors;
  for (unsigned i = 0; i != ofh::NOF_COMPRESSION_TYPES_SUPPORTED; ++i) {
    compressors[i] =
        create_iq_compressor(static_cast<ofh::compression_type>(i), logger, tx_config.iq_scaling * bw_scaling);
  }
  dependencies.compressor_sel = ofh::create_iq_compressor_selector(std::move(compressors));

  dependencies.up_builder =
      (tx_config.is_downlink_static_compr_hdr_enabled)
          ? ofh::create_static_compr_method_ofh_user_plane_packet_builder(logger, *dependencies.compressor_sel)
          : ofh::create_dynamic_compr_method_ofh_user_plane_packet_builder(logger, *dependencies.compressor_sel);

  return std::make_unique<data_flow_uplane_downlink_data_impl>(config, std::move(dependencies));
}

static std::unique_ptr<downlink_manager>
create_downlink_manager(const transmitter_config&                         tx_config,
                        srslog::basic_logger&                             logger,
                        std::shared_ptr<ether::eth_frame_pool>            frame_pool,
                        std::shared_ptr<uplink_cplane_context_repository> ul_cp_context_repo,
                        const std::vector<task_executor*>&                executors)
{
  std::vector<data_flow_uplane_downlink_task_dispatcher_entry> df_uplane_task_dispatcher_cfg;
  std::vector<data_flow_cplane_downlink_task_dispatcher_entry> df_cplane_task_dispatcher_cfg;
  for (auto* executor : executors) {
    df_cplane_task_dispatcher_cfg.emplace_back(
        create_data_flow_cplane_sched(tx_config, logger, frame_pool, ul_cp_context_repo), *executor);
    df_uplane_task_dispatcher_cfg.emplace_back(create_data_flow_uplane_data(tx_config, logger, frame_pool), *executor);
  }

  auto data_flow_cplane =
      std::make_unique<data_flow_cplane_downlink_task_dispatcher>(std::move(df_cplane_task_dispatcher_cfg));
  auto data_flow_uplane =
      std::make_unique<data_flow_uplane_downlink_task_dispatcher>(std::move(df_uplane_task_dispatcher_cfg));

  if (tx_config.downlink_broadcast) {
    downlink_handler_broadcast_impl_config dl_config;
    dl_config.dl_eaxc            = tx_config.dl_eaxc;
    dl_config.tdd_config         = tx_config.tdd_config;
    dl_config.cp                 = tx_config.cp;
    dl_config.scs                = tx_config.scs;
    dl_config.dl_processing_time = tx_config.dl_processing_time;
    dl_config.tx_timing_params   = tx_config.symbol_handler_cfg.tx_timing_params;

    downlink_handler_broadcast_impl_dependencies dl_dependencies;
    dl_dependencies.logger           = &logger;
    dl_dependencies.data_flow_cplane = std::move(data_flow_cplane);
    dl_dependencies.data_flow_uplane = std::move(data_flow_uplane);

    return std::make_unique<downlink_manager_broadcast_impl>(dl_config, std::move(dl_dependencies));
  }

  downlink_handler_impl_config dl_config;
  dl_config.dl_eaxc            = tx_config.dl_eaxc;
  dl_config.tdd_config         = tx_config.tdd_config;
  dl_config.cp                 = tx_config.cp;
  dl_config.scs                = tx_config.scs;
  dl_config.dl_processing_time = tx_config.dl_processing_time;
  dl_config.tx_timing_params   = tx_config.symbol_handler_cfg.tx_timing_params;

  downlink_handler_impl_dependencies dl_dependencies;
  dl_dependencies.logger           = &logger;
  dl_dependencies.data_flow_cplane = std::move(data_flow_cplane);
  dl_dependencies.data_flow_uplane = std::move(data_flow_uplane);
  dl_dependencies.frame_pool_ptr   = frame_pool;

  return std::make_unique<downlink_manager_impl>(dl_config, std::move(dl_dependencies));
}

static std::unique_ptr<uplink_request_handler>
create_uplink_request_handler(const transmitter_config&                         tx_config,
                              srslog::basic_logger&                             logger,
                              std::shared_ptr<ether::eth_frame_pool>            frame_pool,
                              std::shared_ptr<prach_context_repository>         prach_context_repo,
                              std::shared_ptr<uplink_context_repository>        ul_slot_context_repo,
                              std::shared_ptr<uplink_cplane_context_repository> ul_cp_context_repo)
{
  uplink_request_handler_impl_config config;
  config.is_prach_cp_enabled = tx_config.is_prach_cp_enabled;
  config.prach_eaxc          = tx_config.prach_eaxc;
  config.ul_data_eaxc        = tx_config.ul_eaxc;
  config.tdd_config          = tx_config.tdd_config;
  config.cp                  = tx_config.cp;

  uplink_request_handler_impl_dependencies dependencies;
  dependencies.logger        = &logger;
  dependencies.ul_slot_repo  = ul_slot_context_repo;
  dependencies.ul_prach_repo = prach_context_repo;
  dependencies.data_flow     = create_data_flow_cplane_sched(tx_config, logger, frame_pool, ul_cp_context_repo);

  return std::make_unique<uplink_request_handler_impl>(config, std::move(dependencies));
}

static std::shared_ptr<ether::eth_frame_pool> create_eth_frame_pool(const transmitter_config& tx_config,
                                                                    srslog::basic_logger&     logger)
{
  auto eth_builder   = ether::create_vlan_frame_builder();
  auto ecpri_builder = ecpri::create_ecpri_packet_builder();

  std::array<std::unique_ptr<ofh::iq_compressor>, ofh::NOF_COMPRESSION_TYPES_SUPPORTED> compressors;
  for (unsigned i = 0; i != ofh::NOF_COMPRESSION_TYPES_SUPPORTED; ++i) {
    compressors[i] = create_iq_compressor(ofh::compression_type::none, logger);
  }
  auto compressor_sel = ofh::create_iq_compressor_selector(std::move(compressors));

  std::unique_ptr<uplane_message_builder> uplane_builder =
      (tx_config.is_downlink_static_compr_hdr_enabled)
          ? ofh::create_static_compr_method_ofh_user_plane_packet_builder(logger, *compressor_sel)
          : ofh::create_dynamic_compr_method_ofh_user_plane_packet_builder(logger, *compressor_sel);

  units::bytes headers_size = eth_builder->get_header_size() +
                              ecpri_builder->get_header_size(ecpri::message_type::iq_data) +
                              uplane_builder->get_header_size(tx_config.dl_compr_params);

  unsigned nof_prbs =
      get_max_Nprb(bs_channel_bandwidth_to_MHz(tx_config.ru_working_bw), tx_config.scs, srsran::frequency_range::FR1);

  unsigned nof_frames_per_symbol = ofh_uplane_fragment_size_calculator::calculate_nof_segments(
      tx_config.mtu_size, nof_prbs, tx_config.dl_compr_params, headers_size);

  return std::make_shared<ether::eth_frame_pool>(tx_config.mtu_size, nof_frames_per_symbol);
}

static transmitter_impl_dependencies
resolve_transmitter_dependencies(const transmitter_config&                         tx_config,
                                 srslog::basic_logger&                             logger,
                                 task_executor&                                    tx_executor,
                                 const std::vector<task_executor*>&                downlink_executors,
                                 std::unique_ptr<ether::gateway>                   eth_gateway,
                                 std::shared_ptr<prach_context_repository>         prach_context_repo,
                                 std::shared_ptr<uplink_context_repository>        ul_slot_context_repo,
                                 std::shared_ptr<uplink_cplane_context_repository> ul_cp_context_repo)
{
  transmitter_impl_dependencies dependencies;

  dependencies.logger   = &logger;
  dependencies.executor = &tx_executor;

  auto frame_pool = create_eth_frame_pool(tx_config, logger);

  dependencies.dl_manager =
      create_downlink_manager(tx_config, logger, frame_pool, ul_cp_context_repo, downlink_executors);

  dependencies.ul_request_handler = std::make_unique<uplink_request_handler_task_dispatcher>(
      create_uplink_request_handler(
          tx_config, logger, frame_pool, prach_context_repo, ul_slot_context_repo, ul_cp_context_repo),
      *downlink_executors.front());

  ether::gw_config eth_cfg;
  eth_cfg.interface                   = tx_config.interface;
  eth_cfg.is_promiscuous_mode_enabled = tx_config.is_promiscuous_mode_enabled;
  eth_cfg.mac_dst_address             = tx_config.mac_dst_address;
  eth_cfg.mtu_size                    = tx_config.mtu_size;
  if (eth_gateway != nullptr) {
    dependencies.eth_gateway = std::move(eth_gateway);
  } else {
#ifdef DPDK_FOUND
    if (tx_config.uses_dpdk) {
      dependencies.eth_gateway = ether::create_dpdk_gateway(eth_cfg, logger);
    } else {
      dependencies.eth_gateway = ether::create_gateway(eth_cfg, logger);
    }
#else
    dependencies.eth_gateway = ether::create_gateway(eth_cfg, logger);
#endif
  }

  dependencies.frame_pool = frame_pool;

  return dependencies;
}

std::unique_ptr<transmitter>
srsran::ofh::create_transmitter(const transmitter_config&                         transmitter_cfg,
                                srslog::basic_logger&                             logger,
                                task_executor&                                    tx_executor,
                                const std::vector<task_executor*>&                downlink_executors,
                                std::unique_ptr<ether::gateway>                   eth_gateway,
                                std::shared_ptr<prach_context_repository>         prach_context_repo,
                                std::shared_ptr<uplink_context_repository>        ul_slot_context_repo,
                                std::shared_ptr<uplink_cplane_context_repository> ul_cp_context_repo)
{
  return std::make_unique<transmitter_impl>(transmitter_cfg,
                                            resolve_transmitter_dependencies(transmitter_cfg,
                                                                             logger,
                                                                             tx_executor,
                                                                             downlink_executors,
                                                                             std::move(eth_gateway),
                                                                             prach_context_repo,
                                                                             ul_slot_context_repo,
                                                                             ul_cp_context_repo));
}
