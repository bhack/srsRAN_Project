
#ifndef SRSGNB_RLC_MANAGER_FACTORY_H
#define SRSGNB_RLC_MANAGER_FACTORY_H

#include "rlc.h"
#include <memory>

namespace srsgnb {

/// Creates an instance of a RLC UL bearer
std::unique_ptr<rlc_ul_bearer> create_rlc_ul_bearer(du_ue_index_t ue_index, lcid_t lcid, rlc_ul_sdu_notifier& notifier);

} // namespace srsgnb

#endif // SRSGNB_RLC_MANAGER_FACTORY_H
