
#ifndef SRSGNB_F1AP_DU_H
#define SRSGNB_F1AP_DU_H

#include "srsgnb/adt/byte_buffer.h"
#include "srsgnb/ran/du_types.h"
#include "srsgnb/ran/lcid.h"

namespace srsgnb {

struct ul_ccch_indication_message;
struct ul_rrc_transfer_message;
struct du_ue_create_response_message;

class f1ap_du_dl_interface
{
public:
  virtual ~f1ap_du_dl_interface()               = default;
  virtual void push_sdu(const byte_buffer& sdu) = 0;
};

struct ul_rrc_message {
  du_cell_index_t cell_index;
  du_ue_index_t   ue_index;
  lcid_t          lcid;
  byte_buffer     rrc_msg;
};

struct ul_rrc_message_delivery_status {
  du_cell_index_t cell_index;
  du_ue_index_t   ue_index;
  lcid_t          lcid;
  bool            rrc_delivery_status;
};

class f1ap_du_ul_interface
{
public:
  virtual ~f1ap_du_ul_interface()                                                           = default;
  virtual void ul_ccch_message_indication(const ul_ccch_indication_message& msg)            = 0;
  virtual void ul_rrc_message_transfer(const ul_rrc_transfer_message& msg)                  = 0;
  virtual void ul_rrc_message_delivery_report(const ul_rrc_message_delivery_status& report) = 0;
};

class f1ap_du_config_interface
{
public:
  virtual ~f1ap_du_config_interface()                                          = default;
  virtual void ue_creation_response(const du_ue_create_response_message& resp) = 0;
};

/// Packet entry point for the F1AP interface.
class f1ap_du_interface : public f1ap_du_dl_interface, public f1ap_du_ul_interface, public f1ap_du_config_interface
{
public:
  virtual ~f1ap_du_interface() = default;
};

/// Packet notification interface.
class f1ap_du_pdu_notifier
{
public:
  virtual ~f1ap_du_pdu_notifier() = default;
  /// This callback is invoked for each outgoing packed that is generated by the F1AP interface.
  virtual void push_pdu(const byte_buffer& data) = 0;
};

} // namespace srsgnb

#endif // SRSGNB_F1AP_DU_H
