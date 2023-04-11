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
#include "srsran/gateways/baseband/baseband_gateway_base.h"
#include "srsran/gateways/baseband/baseband_gateway_timestamp.h"

namespace srsran {

/// Describes a baseband gateway interface for reception.
class baseband_gateway_receiver : public baseband_gateway_base
{
public:
  /// Describes receiver metadata.
  struct metadata {
    /// Indicates the baseband signal received timestamp.
    baseband_gateway_timestamp ts;
  };

  /// \brief Gets the buffer size.
  virtual unsigned get_buffer_size() = 0;

  /// \brief Receives a given baseband buffer.
  /// \param[out,in] data Provides the baseband buffer destination and the number of samples to receive.
  /// \return Receive metadata.
  /// \note The data buffers must the same number of channels than the stream.
  /// \note The application terminates if the stream identifier is invalid.
  virtual metadata receive(baseband_gateway_buffer& data) = 0;
};

} // namespace srsran