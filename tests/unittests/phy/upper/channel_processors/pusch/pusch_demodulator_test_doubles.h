/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "../../../phy_test_utils.h"
#include "srsran/phy/upper/channel_processors/channel_processor_factories.h"
#include "srsran/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "srsran/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "srsran/phy/upper/log_likelihood_ratio.h"
#include <random>

namespace srsran {

class pusch_demodulator_spy : public pusch_demodulator
{
public:
  struct entry_t {
    std::vector<log_likelihood_ratio> codeword;
    dynamic_bit_buffer                scrambling_seq;
    const resource_grid_reader*       grid;
    const channel_estimate*           estimates;
    configuration                     config;
  };

  pusch_demodulator_spy() : llr_dist(log_likelihood_ratio::min().to_int(), log_likelihood_ratio::max().to_int()) {}

  void demodulate(pusch_codeword_buffer&      codeword_buffer,
                  pusch_demodulator_notifier& notifier,
                  const resource_grid_reader& grid,
                  const channel_estimate&     estimates,
                  const configuration&        config) override
  {
    entries.emplace_back();
    entry_t& entry = entries.back();
    entry.codeword.resize(codeword_size);
    entry.scrambling_seq.resize(codeword_size);
    entry.grid      = &grid;
    entry.estimates = &estimates;
    entry.config    = config;

    std::generate(
        entry.codeword.begin(), entry.codeword.end(), [this]() { return log_likelihood_ratio(llr_dist(rgen)); });
    std::generate(entry.scrambling_seq.get_buffer().begin(), entry.scrambling_seq.get_buffer().end(), [this]() {
      return rgen();
    });
    codeword_buffer.on_new_block(entry.codeword, entry.scrambling_seq);
    codeword_buffer.on_end_codeword();

    // Report empty demodulation statistics.
    notifier.on_end_stats(pusch_demodulator_notifier::demodulation_stats());
  }

  void set_codeword_size(unsigned codeword_size_) { codeword_size = codeword_size_; }

  const std::vector<entry_t>& get_entries() const { return entries; }

  void clear() { entries.clear(); }

private:
  unsigned                                                        codeword_size = 0;
  std::vector<entry_t>                                            entries;
  std::mt19937                                                    rgen;
  std::uniform_int_distribution<log_likelihood_ratio::value_type> llr_dist;
};

PHY_SPY_FACTORY_TEMPLATE(pusch_demodulator);

} // namespace srsran
