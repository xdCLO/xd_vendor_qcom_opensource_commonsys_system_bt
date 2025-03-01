/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  this file contains the main GATT client functions
 *
 ******************************************************************************/

#include "bt_target.h"

#include <string.h>
#include "bt_common.h"
#include "bt_utils.h"
#include "gatt_int.h"
#include "l2c_int.h"
#include "log/log.h"
#include "osi/include/osi.h"

#define GATT_WRITE_LONG_HDR_SIZE 5 /* 1 opcode + 2 handle + 2 offset */
#define GATT_READ_CHAR_VALUE_HDL (GATT_READ_CHAR_VALUE | 0x80)
#define GATT_READ_INC_SRV_UUID128 (GATT_DISC_INC_SRVC | 0x90)

#define GATT_PREP_WRITE_RSP_MIN_LEN 4
#define GATT_NOTIFICATION_MIN_LEN 2
#define GATT_WRITE_RSP_MIN_LEN 2
#define GATT_INFO_RSP_MIN_LEN 1
#define GATT_MTU_RSP_MIN_LEN 2
#define GATT_READ_BY_TYPE_RSP_MIN_LEN 1

using base::StringPrintf;
using bluetooth::Uuid;

/*******************************************************************************
 *                      G L O B A L      G A T T       D A T A                 *
 ******************************************************************************/
void gatt_send_prepare_write(tGATT_TCB& tcb, tGATT_CLCB* p_clcb);

uint8_t disc_type_to_att_opcode[GATT_DISC_MAX] = {
    0,
    GATT_REQ_READ_BY_GRP_TYPE, /*  GATT_DISC_SRVC_ALL = 1, */
    GATT_REQ_FIND_TYPE_VALUE,  /*  GATT_DISC_SRVC_BY_UUID,  */
    GATT_REQ_READ_BY_TYPE,     /*  GATT_DISC_INC_SRVC,      */
    GATT_REQ_READ_BY_TYPE,     /*  GATT_DISC_CHAR,          */
    GATT_REQ_FIND_INFO         /*  GATT_DISC_CHAR_DSCPT,    */
};

uint16_t disc_type_to_uuid[GATT_DISC_MAX] = {
    0,                         /* reserved */
    GATT_UUID_PRI_SERVICE,     /* <service> DISC_SRVC_ALL */
    GATT_UUID_PRI_SERVICE,     /* <service> for DISC_SERVC_BY_UUID */
    GATT_UUID_INCLUDE_SERVICE, /* <include_service> for DISC_INC_SRVC */
    GATT_UUID_CHAR_DECLARE,    /* <characteristic> for DISC_CHAR */
    0                          /* no type filtering for DISC_CHAR_DSCPT */
};

/*******************************************************************************
 *
 * Function         gatt_act_discovery
 *
 * Description      GATT discovery operation.
 *
 * Returns          void.
 *
 ******************************************************************************/
void gatt_act_discovery(tGATT_CLCB* p_clcb) {
  uint8_t op_code = disc_type_to_att_opcode[p_clcb->op_subtype];

  if (p_clcb->s_handle > p_clcb->e_handle || p_clcb->s_handle == 0) {
    /* end of handle range */
    gatt_end_operation(p_clcb, GATT_SUCCESS, NULL);
    return;
  }

  tGATT_CL_MSG cl_req;
  memset(&cl_req, 0, sizeof(tGATT_CL_MSG));

  cl_req.browse.s_handle = p_clcb->s_handle;
  cl_req.browse.e_handle = p_clcb->e_handle;

  if (disc_type_to_uuid[p_clcb->op_subtype] != 0) {
    cl_req.browse.uuid =
        bluetooth::Uuid::From16Bit(disc_type_to_uuid[p_clcb->op_subtype]);
  }

    if (p_clcb->op_subtype ==
        GATT_DISC_SRVC_BY_UUID) /* fill in the FindByTypeValue request info*/
    {
      cl_req.find_type_value.uuid =
          bluetooth::Uuid::From16Bit(disc_type_to_uuid[p_clcb->op_subtype]);
      cl_req.find_type_value.s_handle = p_clcb->s_handle;
      cl_req.find_type_value.e_handle = p_clcb->e_handle;

      size_t size = p_clcb->uuid.GetShortestRepresentationSize();
      cl_req.find_type_value.value_len = size;
      if (size == Uuid::kNumBytes16) {
        uint8_t* p = cl_req.find_type_value.value;
        UINT16_TO_STREAM(p, p_clcb->uuid.As16Bit());
      } else if (size == Uuid::kNumBytes32) {
        /* if service type is 32 bits UUID, convert it now */
        memcpy(cl_req.find_type_value.value, p_clcb->uuid.To128BitLE().data(),
              Uuid::kNumBytes128);
        cl_req.find_type_value.value_len = Uuid::kNumBytes128;
      } else
        memcpy(cl_req.find_type_value.value, p_clcb->uuid.To128BitLE().data(),
               size);
    }

  tGATT_STATUS st = attp_send_cl_msg(*p_clcb->p_tcb, p_clcb, op_code, &cl_req);
  if (st != GATT_SUCCESS && st != GATT_CMD_STARTED) {
    gatt_end_operation(p_clcb, GATT_ERROR, NULL);
  }
}

/*******************************************************************************
 *
 * Function         gatt_act_read
 *
 * Description      GATT read operation.
 *
 * Returns          void.
 *
 ******************************************************************************/
void gatt_act_read(tGATT_CLCB* p_clcb, uint16_t offset) {
  tGATT_TCB& tcb = *p_clcb->p_tcb;
  uint8_t rt = GATT_INTERNAL_ERROR;
  tGATT_CL_MSG msg;
  uint8_t op_code = 0;

  memset(&msg, 0, sizeof(tGATT_CL_MSG));

  switch (p_clcb->op_subtype) {
    case GATT_READ_CHAR_VALUE:
    case GATT_READ_BY_TYPE:
      op_code = GATT_REQ_READ_BY_TYPE;
      msg.browse.s_handle = p_clcb->s_handle;
      msg.browse.e_handle = p_clcb->e_handle;
      if (p_clcb->op_subtype == GATT_READ_BY_TYPE)
        msg.browse.uuid = p_clcb->uuid;
      else {
        msg.browse.uuid = bluetooth::Uuid::From16Bit(GATT_UUID_CHAR_DECLARE);
      }
      break;

    case GATT_READ_CHAR_VALUE_HDL:
    case GATT_READ_BY_HANDLE:
      if (!p_clcb->counter) {
        op_code = GATT_REQ_READ;
        msg.handle = p_clcb->s_handle;
      } else {
        if (!p_clcb->first_read_blob_after_read)
          p_clcb->first_read_blob_after_read = true;
        else
          p_clcb->first_read_blob_after_read = false;

        VLOG(1) << __func__ << ": first_read_blob_after_read="
                << p_clcb->first_read_blob_after_read;
        op_code = GATT_REQ_READ_BLOB;
        msg.read_blob.offset = offset;
        msg.read_blob.handle = p_clcb->s_handle;
      }
      p_clcb->op_subtype &= ~0x80;
      break;

    case GATT_READ_PARTIAL:
      op_code = GATT_REQ_READ_BLOB;
      msg.read_blob.handle = p_clcb->s_handle;
      msg.read_blob.offset = offset;
      break;

    case GATT_READ_MULTIPLE:
      op_code = GATT_REQ_READ_MULTI;
      memcpy(&msg.read_multi, p_clcb->p_attr_buf, sizeof(tGATT_READ_MULTI));
      break;

    case GATT_READ_INC_SRV_UUID128:
      op_code = GATT_REQ_READ;
      msg.handle = p_clcb->s_handle;
      p_clcb->op_subtype &= ~0x90;
      break;

    default:
      LOG(ERROR) << "Unknown read type:" << +p_clcb->op_subtype;
      break;
  }

  if (op_code != 0) rt = attp_send_cl_msg(tcb, p_clcb, op_code, &msg);

  if (op_code == 0 || (rt != GATT_SUCCESS && rt != GATT_CMD_STARTED)) {
    gatt_end_operation(p_clcb, rt, NULL);
  }
}

/** GATT write operation */
void gatt_act_write(tGATT_CLCB* p_clcb, uint8_t sec_act) {
  tGATT_TCB& tcb = *p_clcb->p_tcb;

  CHECK(p_clcb->p_attr_buf);
  tGATT_VALUE& attr = *((tGATT_VALUE*)p_clcb->p_attr_buf);

  switch (p_clcb->op_subtype) {
    case GATT_WRITE_NO_RSP: {
      p_clcb->s_handle = attr.handle;
      uint8_t op_code = (sec_act == GATT_SEC_SIGN_DATA) ? GATT_SIGN_CMD_WRITE
                                                        : GATT_CMD_WRITE;
      uint8_t rt = gatt_send_write_msg(tcb, p_clcb, op_code, attr.handle,
                                       attr.len, 0, attr.value);
      if (rt != GATT_CMD_STARTED) {
        if (rt != GATT_SUCCESS) {
          LOG(ERROR) << StringPrintf(
              "gatt_act_write() failed op_code=0x%x rt=%d", op_code, rt);
        }
        gatt_end_operation(p_clcb, rt, NULL);
      }
      return;
    }

    case GATT_WRITE: {
      if (attr.len <= (tcb.payload_size - GATT_HDR_SIZE)) {
        p_clcb->s_handle = attr.handle;

        uint8_t rt = gatt_send_write_msg(tcb, p_clcb, GATT_REQ_WRITE,
                                         attr.handle, attr.len, 0, attr.value);
        if (rt != GATT_SUCCESS && rt != GATT_CMD_STARTED &&
            rt != GATT_CONGESTED) {
          if (rt != GATT_SUCCESS) {
            LOG(ERROR) << StringPrintf(
                "gatt_act_write() failed op_code=0x%x rt=%d", GATT_REQ_WRITE,
                rt);
          }
          gatt_end_operation(p_clcb, rt, NULL);
        }

      } else {
        /* prepare write for long attribute */
        gatt_send_prepare_write(tcb, p_clcb);
      }
      return;
    }

    case GATT_WRITE_PREPARE:
      gatt_send_prepare_write(tcb, p_clcb);
      return;

    default:
      CHECK(false) << "Unknown write type" << p_clcb->op_subtype;
      return;
  }
}
/*******************************************************************************
 *
 * Function         gatt_send_queue_write_cancel
 *
 * Description      send queue write cancel
 *
 * Returns          void.
 *
 ******************************************************************************/
void gatt_send_queue_write_cancel(tGATT_TCB& tcb, tGATT_CLCB* p_clcb,
                                  tGATT_EXEC_FLAG flag) {
  uint8_t rt;

  VLOG(1) << __func__;

  tGATT_CL_MSG gatt_cl_msg;
  gatt_cl_msg.exec_write = flag;
  rt = attp_send_cl_msg(tcb, p_clcb, GATT_REQ_EXEC_WRITE, &gatt_cl_msg);

  if (rt != GATT_SUCCESS) {
    gatt_end_operation(p_clcb, rt, NULL);
  }
}
/*******************************************************************************
 *
 * Function         gatt_check_write_long_terminate
 *
 * Description      To terminate write long or not.
 *
 * Returns          true: write long is terminated; false keep sending.
 *
 ******************************************************************************/
bool gatt_check_write_long_terminate(tGATT_TCB& tcb, tGATT_CLCB* p_clcb,
                                     tGATT_VALUE* p_rsp_value) {
  tGATT_VALUE* p_attr = (tGATT_VALUE*)p_clcb->p_attr_buf;
  bool terminate = false;
  tGATT_EXEC_FLAG flag = GATT_PREP_WRITE_EXEC;

  VLOG(1) << __func__;
  /* check the first write response status */
  if ((p_rsp_value != NULL) && (p_attr != NULL)) {
    if (p_rsp_value->handle != p_attr->handle ||
        p_rsp_value->len != p_clcb->counter ||
        memcmp(p_rsp_value->value, p_attr->value + p_attr->offset,
               p_rsp_value->len)) {
      /* data does not match    */
      p_clcb->status = GATT_ERROR;
      flag = GATT_PREP_WRITE_CANCEL;
      terminate = true;
    } else /* response checking is good */
    {
      p_clcb->status = GATT_SUCCESS;
      /* update write offset and check if end of attribute value */
      if ((p_attr->offset += p_rsp_value->len) >= p_attr->len) terminate = true;
    }
  } else {
    VLOG(1) << __func__ << " packet corrupted: cancel the prepare write";
    p_clcb->status = GATT_ERROR;
    flag = GATT_PREP_WRITE_CANCEL;
    p_clcb->op_subtype = GATT_REQ_EXEC_WRITE;
    terminate = true;
  }
  if (terminate && p_clcb->op_subtype != GATT_WRITE_PREPARE) {
    gatt_send_queue_write_cancel(tcb, p_clcb, flag);
  }
  return terminate;
}

/** Send prepare write */
void gatt_send_prepare_write(tGATT_TCB& tcb, tGATT_CLCB* p_clcb) {
  tGATT_VALUE* p_attr = (tGATT_VALUE*)p_clcb->p_attr_buf;
  uint8_t type = p_clcb->op_subtype;

  VLOG(1) << __func__ << StringPrintf(" type=0x%x", type);
  uint16_t to_send = p_attr->len - p_attr->offset;

  if (to_send > (tcb.payload_size -
                 GATT_WRITE_LONG_HDR_SIZE)) /* 2 = uint16_t offset bytes  */
    to_send = tcb.payload_size - GATT_WRITE_LONG_HDR_SIZE;

  p_clcb->s_handle = p_attr->handle;

  uint16_t offset = p_attr->offset;
  if (type == GATT_WRITE_PREPARE) {
    offset += p_clcb->start_offset;
  }

  VLOG(1) << StringPrintf("offset =0x%x len=%d", offset, to_send);

  uint8_t rt = gatt_send_write_msg(tcb, p_clcb, GATT_REQ_PREPARE_WRITE,
                                   p_attr->handle, to_send, /* length */
                                   offset,                  /* used as offset */
                                   p_attr->value + p_attr->offset); /* data */

  /* remember the write long attribute length */
  p_clcb->counter = to_send;

  if (rt != GATT_SUCCESS && rt != GATT_CMD_STARTED && rt != GATT_CONGESTED) {
    gatt_end_operation(p_clcb, rt, NULL);
  }
}

/*******************************************************************************
 *
 * Function         gatt_process_find_type_value_rsp
 *
 * Description      This function handles the find by type value response.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_find_type_value_rsp(UNUSED_ATTR tGATT_TCB& tcb,
                                      tGATT_CLCB* p_clcb, uint16_t len,
                                      uint8_t* p_data) {
  tGATT_DISC_RES result;
  uint8_t* p = p_data;

  VLOG(1) << __func__;
  /* unexpected response */
  if (p_clcb->operation != GATTC_OPTYPE_DISCOVERY ||
      p_clcb->op_subtype != GATT_DISC_SRVC_BY_UUID)
    return;

  memset(&result, 0, sizeof(tGATT_DISC_RES));
  result.type = bluetooth::Uuid::From16Bit(GATT_UUID_PRI_SERVICE);

  /* returns a series of handle ranges */
  while (len >= 4) {
    STREAM_TO_UINT16(result.handle, p);
    STREAM_TO_UINT16(result.value.group_value.e_handle, p);
    result.value.group_value.service_type = p_clcb->uuid;

    len -= 4;

    if (p_clcb->p_reg->app_cb.p_disc_res_cb)
      (*p_clcb->p_reg->app_cb.p_disc_res_cb)(p_clcb->conn_id,
                                             p_clcb->op_subtype, &result);
  }

  /* last handle  + 1 */
  p_clcb->s_handle = (result.value.group_value.e_handle == 0)
                         ? 0
                         : (result.value.group_value.e_handle + 1);
  /* initiate another request */
  gatt_act_discovery(p_clcb);
}
/*******************************************************************************
 *
 * Function         gatt_process_read_info_rsp
 *
 * Description      This function is called to handle the read information
 *                  response.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_read_info_rsp(UNUSED_ATTR tGATT_TCB& tcb, tGATT_CLCB* p_clcb,
                                UNUSED_ATTR uint8_t op_code, uint16_t len,
                                uint8_t* p_data) {
  tGATT_DISC_RES result;
  uint8_t *p = p_data, uuid_len = 0, type;

  if (len < GATT_INFO_RSP_MIN_LEN) {
    LOG(ERROR) << "invalid Info Response PDU received, discard.";
    gatt_end_operation(p_clcb, GATT_INVALID_PDU, NULL);
    return;
  }
  /* unexpected response */
  if (p_clcb->operation != GATTC_OPTYPE_DISCOVERY ||
      p_clcb->op_subtype != GATT_DISC_CHAR_DSCPT)
    return;

  STREAM_TO_UINT8(type, p);
  len -= 1;

  if (type == GATT_INFO_TYPE_PAIR_16)
    uuid_len = Uuid::kNumBytes16;
  else if (type == GATT_INFO_TYPE_PAIR_128)
    uuid_len = Uuid::kNumBytes128;

  while (len >= uuid_len + 2) {
    STREAM_TO_UINT16(result.handle, p);

    if (uuid_len > 0) {
      if (!gatt_parse_uuid_from_cmd(&result.type, uuid_len, &p)) break;
    } else
      result.type = p_clcb->uuid;

    len -= (uuid_len + 2);

    if (p_clcb->p_reg->app_cb.p_disc_res_cb)
      (*p_clcb->p_reg->app_cb.p_disc_res_cb)(p_clcb->conn_id,
                                             p_clcb->op_subtype, &result);
  }

  p_clcb->s_handle = (result.handle == 0) ? 0 : (result.handle + 1);
  /* initiate another request */
  gatt_act_discovery(p_clcb);
}
/*******************************************************************************
 *
 * Function         gatt_proc_disc_error_rsp
 *
 * Description      Process the read by type response and send another request
 *                  if needed.
 *
 * Returns          void.
 *
 ******************************************************************************/
void gatt_proc_disc_error_rsp(UNUSED_ATTR tGATT_TCB& tcb, tGATT_CLCB* p_clcb,
                              uint8_t opcode, UNUSED_ATTR uint16_t handle,
                              uint8_t reason) {
  tGATT_STATUS status = (tGATT_STATUS)reason;

  VLOG(1) << __func__
          << StringPrintf("reason: %02x cmd_code %04x", reason, opcode);

  switch (opcode) {
    case GATT_REQ_READ_BY_GRP_TYPE:
    case GATT_REQ_FIND_TYPE_VALUE:
    case GATT_REQ_READ_BY_TYPE:
    case GATT_REQ_FIND_INFO:
      if (reason == GATT_NOT_FOUND) {
        status = GATT_SUCCESS;
        VLOG(1) << "Discovery completed";
      }
      break;
    default:
      LOG(ERROR) << StringPrintf("Incorrect discovery opcode %04x", opcode);
      break;
  }

  gatt_end_operation(p_clcb, status, NULL);
}

/*******************************************************************************
 *
 * Function         gatt_process_error_rsp
 *
 * Description      This function is called to handle the error response
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_error_rsp(tGATT_TCB& tcb, tGATT_CLCB* p_clcb,
                            UNUSED_ATTR uint8_t op_code,
                            UNUSED_ATTR uint16_t len, uint8_t* p_data) {
  uint8_t opcode, reason, *p = p_data;
  uint16_t handle;
  tGATT_VALUE* p_attr = (tGATT_VALUE*)p_clcb->p_attr_buf;

  VLOG(1) << __func__;

  if (len < 4) {
    android_errorWriteLog(0x534e4554, "79591688");
    LOG(ERROR) << "Error response too short";
    // Specification does not clearly define what should happen if error
    // response is too short. General rule in BT Spec 5.0 Vol 3, Part F 3.4.1.1
    // is: "If an error code is received in the Error Response that is not
    // understood by the client, for example an error code that was reserved for
    // future use that is now being used in a future version of this
    // specification, then the Error Response shall still be considered to state
    // that the given request cannot be performed for an unknown reason."
    opcode = handle = 0;
    reason = 0x7F;
  } else {
    STREAM_TO_UINT8(opcode, p);
    STREAM_TO_UINT16(handle, p);
    STREAM_TO_UINT8(reason, p);
  }

  if (p_clcb->operation == GATTC_OPTYPE_DISCOVERY) {
    gatt_proc_disc_error_rsp(tcb, p_clcb, opcode, handle, reason);
  } else {
    if ((p_clcb->operation == GATTC_OPTYPE_WRITE) &&
        (p_clcb->op_subtype == GATT_WRITE) &&
        (opcode == GATT_REQ_PREPARE_WRITE) && (p_attr) &&
        (handle == p_attr->handle)) {
      p_clcb->status = reason;
      gatt_send_queue_write_cancel(tcb, p_clcb, GATT_PREP_WRITE_CANCEL);
    } else if ((p_clcb->operation == GATTC_OPTYPE_READ) &&
               ((p_clcb->op_subtype == GATT_READ_CHAR_VALUE_HDL) ||
                (p_clcb->op_subtype == GATT_READ_BY_HANDLE)) &&
               (opcode == GATT_REQ_READ_BLOB) &&
               p_clcb->first_read_blob_after_read &&
               (reason == GATT_NOT_LONG)) {
      gatt_end_operation(p_clcb, GATT_SUCCESS, (void*)p_clcb->p_attr_buf);
    } else
      gatt_end_operation(p_clcb, reason, NULL);
  }
}
/*******************************************************************************
 *
 * Function         gatt_process_prep_write_rsp
 *
 * Description      This function is called to handle the read response
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_prep_write_rsp(tGATT_TCB& tcb, tGATT_CLCB* p_clcb,
                                 uint8_t op_code, uint16_t len,
                                 uint8_t* p_data) {
  uint8_t* p = p_data;

  tGATT_VALUE value = {
      .conn_id = p_clcb->conn_id, .auth_req = GATT_AUTH_REQ_NONE,
  };

  LOG(ERROR) << StringPrintf("value resp op_code = %s len = %d",
                             gatt_dbg_op_name(op_code), len);

  if (len < GATT_PREP_WRITE_RSP_MIN_LEN) {
    LOG(ERROR) << "illegal prepare write response length, discard";
    gatt_end_operation(p_clcb, GATT_INVALID_PDU, &value);
    return;
  }

  STREAM_TO_UINT16(value.handle, p);
  STREAM_TO_UINT16(value.offset, p);

  value.len = len - 4;

  memcpy(value.value, p, value.len);

  if (!gatt_check_write_long_terminate(tcb, p_clcb, &value)) {
    gatt_send_prepare_write(tcb, p_clcb);
    return;
  }

  if (p_clcb->op_subtype == GATT_WRITE_PREPARE) {
    /* application should verify handle offset
       and value are matched or not */
    gatt_end_operation(p_clcb, p_clcb->status, &value);
  }
}
/*******************************************************************************
 *
 * Function         gatt_process_notification
 *
 * Description      Handle the handle value indication/notification.
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_notification(tGATT_TCB& tcb, uint8_t op_code, uint16_t len,
                               uint8_t* p_data) {
  tGATT_VALUE value;
  tGATT_REG* p_reg;
  uint16_t conn_id;
  tGATT_STATUS encrypt_status;
  uint8_t* p = p_data;
  uint8_t i;
  uint8_t event = (op_code == GATT_HANDLE_VALUE_NOTIF)
                      ? GATTC_OPTYPE_NOTIFICATION
                      : GATTC_OPTYPE_INDICATION;

  VLOG(1) << __func__;

  if (len < GATT_NOTIFICATION_MIN_LEN) {
    LOG(ERROR) << "illegal notification PDU length, discard";
    return;
  }

  memset(&value, 0, sizeof(value));
  STREAM_TO_UINT16(value.handle, p);
  value.len = len - 2;
  if (value.len > GATT_MAX_ATTR_LEN) {
    LOG(ERROR) << "value.len larger than GATT_MAX_ATTR_LEN, discard";
    return;
  }
  memcpy(value.value, p, value.len);

  if (!GATT_HANDLE_IS_VALID(value.handle)) {
    /* illegal handle, send ack now */
    if (op_code == GATT_HANDLE_VALUE_IND)
      attp_send_cl_msg(tcb, nullptr, GATT_HANDLE_VALUE_CONF, NULL);
    return;
  }

  if (event == GATTC_OPTYPE_INDICATION) {
    if (tcb.ind_count) {
      /* this is an error case that receiving an indication but we
         still has an indication not being acked yet.
         For now, just log the error reset the counter.
         Later we need to disconnect the link unconditionally.
      */
      LOG(ERROR) << __func__ << " rcv Ind. but ind_count=" << tcb.ind_count
                 << " (will reset ind_count)";
    }
    tcb.ind_count = 0;
  }

  /* should notify all registered client with the handle value
     notificaion/indication
     Note: need to do the indication count and start timer first then do
     callback
   */

  for (i = 0, p_reg = gatt_cb.cl_rcb; i < GATT_MAX_APPS; i++, p_reg++) {
    if (p_reg->in_use && p_reg->app_cb.p_cmpl_cb &&
        (event == GATTC_OPTYPE_INDICATION))
      tcb.ind_count++;
  }

  if (event == GATTC_OPTYPE_INDICATION) {
    /* start a timer for app confirmation */
    if (tcb.ind_count > 0)
      gatt_start_ind_ack_timer(tcb);
    else /* no app to indicate, or invalid handle */
      attp_send_cl_msg(tcb, nullptr, GATT_HANDLE_VALUE_CONF, NULL);
  }

  encrypt_status = gatt_get_link_encrypt_status(tcb);
  tGATT_CL_COMPLETE gatt_cl_complete;
  gatt_cl_complete.att_value = value;
  for (i = 0, p_reg = gatt_cb.cl_rcb; i < GATT_MAX_APPS; i++, p_reg++) {
    if (p_reg->in_use && p_reg->app_cb.p_cmpl_cb) {
      conn_id = GATT_CREATE_CONN_ID(tcb.tcb_idx, p_reg->gatt_if);
      (*p_reg->app_cb.p_cmpl_cb)(conn_id, event, encrypt_status,
                                 &gatt_cl_complete);
    }
  }
}

/*******************************************************************************
 *
 * Function         gatt_process_read_by_type_rsp
 *
 * Description      This function is called to handle the read by type response.
 *                  read by type can be used for discovery, or read by type or
 *                  read characteristic value.
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_read_by_type_rsp(tGATT_TCB& tcb, tGATT_CLCB* p_clcb,
                                   uint8_t op_code, uint16_t len,
                                   uint8_t* p_data) {
  tGATT_DISC_RES result;
  tGATT_DISC_VALUE record_value;
  uint8_t *p = p_data, value_len, handle_len = 2;
  uint16_t handle = 0;

  /* discovery procedure and no callback function registered */
  if (((!p_clcb->p_reg) || (!p_clcb->p_reg->app_cb.p_disc_res_cb)) &&
      (p_clcb->operation == GATTC_OPTYPE_DISCOVERY))
    return;

  if (len < GATT_READ_BY_TYPE_RSP_MIN_LEN) {
    LOG(ERROR) << "Illegal ReadByType/ReadByGroupType Response length, discard";
    gatt_end_operation(p_clcb, GATT_INVALID_PDU, NULL);
    return;
  }

  STREAM_TO_UINT8(value_len, p);

  if ((value_len > (tcb.payload_size - 2)) || (value_len > (len - 1))) {
    /* this is an error case that server's response containing a value length
       which is larger than MTU-2
       or value_len > message total length -1 */
    LOG(ERROR) << __func__
               << StringPrintf(
                      ": Discard response op_code=%d "
                      "vale_len=%d > (MTU-2=%d or msg_len-1=%d)",
                      op_code, value_len, (tcb.payload_size - 2), (len - 1));
    gatt_end_operation(p_clcb, GATT_ERROR, NULL);
    return;
  }

  if (op_code == GATT_RSP_READ_BY_GRP_TYPE) handle_len = 4;

  value_len -= handle_len; /* substract the handle pairs bytes */
  len -= 1;

  while (len >= (handle_len + value_len)) {
    STREAM_TO_UINT16(handle, p);

    if (!GATT_HANDLE_IS_VALID(handle)) {
      gatt_end_operation(p_clcb, GATT_INVALID_HANDLE, NULL);
      return;
    }

    memset(&result, 0, sizeof(tGATT_DISC_RES));
    memset(&record_value, 0, sizeof(tGATT_DISC_VALUE));

    result.handle = handle;
    result.type =
        bluetooth::Uuid::From16Bit(disc_type_to_uuid[p_clcb->op_subtype]);

    /* discover all services */
    if (p_clcb->operation == GATTC_OPTYPE_DISCOVERY &&
        p_clcb->op_subtype == GATT_DISC_SRVC_ALL &&
        op_code == GATT_RSP_READ_BY_GRP_TYPE) {
      STREAM_TO_UINT16(handle, p);

      if (!GATT_HANDLE_IS_VALID(handle)) {
        gatt_end_operation(p_clcb, GATT_INVALID_HANDLE, NULL);
        return;
      } else {
        record_value.group_value.e_handle = handle;
        if (!gatt_parse_uuid_from_cmd(&record_value.group_value.service_type,
                                      value_len, &p)) {
          LOG(ERROR) << "discover all service response parsing failure";
          break;
        }
      }
    }
    /* discover included service */
    else if (p_clcb->operation == GATTC_OPTYPE_DISCOVERY &&
             p_clcb->op_subtype == GATT_DISC_INC_SRVC) {
      if (value_len < 4) {
        android_errorWriteLog(0x534e4554, "158833854");
        LOG(ERROR) << __func__ << " Illegal Response length, must be at least 4.";
        gatt_end_operation(p_clcb, GATT_INVALID_PDU, NULL);
        return;
      }
      STREAM_TO_UINT16(record_value.incl_service.s_handle, p);
      STREAM_TO_UINT16(record_value.incl_service.e_handle, p);

      if (!GATT_HANDLE_IS_VALID(record_value.incl_service.s_handle) ||
          !GATT_HANDLE_IS_VALID(record_value.incl_service.e_handle)) {
        gatt_end_operation(p_clcb, GATT_INVALID_HANDLE, NULL);
        return;
      }

      if (value_len == 6) {
        uint16_t tmp;
        STREAM_TO_UINT16(tmp, p);
        record_value.incl_service.service_type =
            bluetooth::Uuid::From16Bit(tmp);
      } else if (value_len == 4) {
        p_clcb->s_handle = record_value.incl_service.s_handle;
        p_clcb->read_uuid128.wait_for_read_rsp = true;
        p_clcb->read_uuid128.next_disc_start_hdl = handle + 1;
        memcpy(&p_clcb->read_uuid128.result, &result, sizeof(result));
        memcpy(&p_clcb->read_uuid128.result.value, &record_value,
               sizeof(result.value));
        p_clcb->op_subtype |= 0x90;
        gatt_act_read(p_clcb, 0);
        return;
      } else {
        LOG(ERROR) << __func__
                   << ": INCL_SRVC failed with invalid data value_len="
                   << +value_len;
        gatt_end_operation(p_clcb, GATT_INVALID_PDU, (void*)p);
        return;
      }
    }
    /* read by type */
    else if (p_clcb->operation == GATTC_OPTYPE_READ &&
             p_clcb->op_subtype == GATT_READ_BY_TYPE) {
      p_clcb->counter = len - 2;
      p_clcb->s_handle = handle;
      if (p_clcb->counter == (p_clcb->p_tcb->payload_size - 4)) {
        p_clcb->op_subtype = GATT_READ_BY_HANDLE;
        if (!p_clcb->p_attr_buf)
          p_clcb->p_attr_buf = (uint8_t*)osi_malloc(GATT_MAX_ATTR_LEN);
        if (p_clcb->counter <= GATT_MAX_ATTR_LEN) {
          memcpy(p_clcb->p_attr_buf, p, p_clcb->counter);
          gatt_act_read(p_clcb, p_clcb->counter);
        } else {
          gatt_end_operation(p_clcb, GATT_INTERNAL_ERROR, (void*)p);
        }
      } else {
        gatt_end_operation(p_clcb, GATT_SUCCESS, (void*)p);
      }
      return;
    } else /* discover characterisitic */
    {
      if (value_len < 3) {
        android_errorWriteLog(0x534e4554, "158778659");
        LOG(ERROR) << __func__ << " Illegal Response length, must be at least 3.";
        gatt_end_operation(p_clcb, GATT_INVALID_PDU, NULL);
        return;
      }
      STREAM_TO_UINT8(record_value.dclr_value.char_prop, p);
      STREAM_TO_UINT16(record_value.dclr_value.val_handle, p);
      if (!GATT_HANDLE_IS_VALID(record_value.dclr_value.val_handle)) {
        gatt_end_operation(p_clcb, GATT_INVALID_HANDLE, NULL);
        return;
      }
      if (!gatt_parse_uuid_from_cmd(&record_value.dclr_value.char_uuid,
                                    (uint16_t)(value_len - 3), &p)) {
        gatt_end_operation(p_clcb, GATT_SUCCESS, NULL);
        /* invalid format, and skip the result */
        return;
      }

      /* UUID not matching */
      if (!p_clcb->uuid.IsEmpty() &&
          !record_value.dclr_value.char_uuid.IsEmpty() &&
          record_value.dclr_value.char_uuid != p_clcb->uuid) {
        len -= (value_len + 2);
        continue; /* skip the result, and look for next one */
      }

      if (p_clcb->operation == GATTC_OPTYPE_READ)
      /* UUID match for read characteristic value */
      {
        /* only read the first matching UUID characteristic value, and
          discard the rest results */
        p_clcb->s_handle = record_value.dclr_value.val_handle;
        p_clcb->op_subtype |= 0x80;
        gatt_act_read(p_clcb, 0);
        return;
      }
    }
    len -= (value_len + handle_len);

    /* result is (handle, 16bits UUID) pairs */
    memcpy(&result.value, &record_value, sizeof(result.value));

    /* send callback if is discover procedure */
    if (p_clcb->operation == GATTC_OPTYPE_DISCOVERY &&
        p_clcb->p_reg->app_cb.p_disc_res_cb)
      (*p_clcb->p_reg->app_cb.p_disc_res_cb)(p_clcb->conn_id,
                                             p_clcb->op_subtype, &result);
  }

  p_clcb->s_handle = (handle == 0) ? 0 : (handle + 1);

  if (p_clcb->operation == GATTC_OPTYPE_DISCOVERY) {
    /* initiate another request */
    gatt_act_discovery(p_clcb);
  } else /* read characteristic value */
  {
    gatt_act_read(p_clcb, 0);
  }
}

/*******************************************************************************
 *
 * Function         gatt_process_read_rsp
 *
 * Description      This function is called to handle the read BLOB response
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_read_rsp(tGATT_TCB& tcb, tGATT_CLCB* p_clcb,
                           UNUSED_ATTR uint8_t op_code, uint16_t len,
                           uint8_t* p_data) {
  uint16_t offset = p_clcb->counter;
  uint8_t* p = p_data;

  if (p_clcb->operation == GATTC_OPTYPE_READ) {
    if (p_clcb->op_subtype != GATT_READ_BY_HANDLE) {
      p_clcb->counter = len;
      gatt_end_operation(p_clcb, GATT_SUCCESS, (void*)p);
    } else {
      /* allocate GKI buffer holding up long attribute value  */
      if (!p_clcb->p_attr_buf)
        p_clcb->p_attr_buf = (uint8_t*)osi_malloc(GATT_MAX_ATTR_LEN);

      /* copy attrobute value into cb buffer  */
      if (offset < GATT_MAX_ATTR_LEN) {
        if ((len + offset) > GATT_MAX_ATTR_LEN)
          len = GATT_MAX_ATTR_LEN - offset;

        p_clcb->counter += len;

        memcpy(p_clcb->p_attr_buf + offset, p, len);

        /* full packet for read or read blob rsp */
        bool packet_is_full;
        if (tcb.payload_size == p_clcb->read_req_current_mtu) {
          packet_is_full = (len == (tcb.payload_size - 1));
        } else {
          packet_is_full = (len == (p_clcb->read_req_current_mtu - 1) ||
                            len == (tcb.payload_size - 1));
          p_clcb->read_req_current_mtu = tcb.payload_size;
        }

        /* send next request if needed  */
        if (packet_is_full && (len + offset < GATT_MAX_ATTR_LEN)) {
          VLOG(1) << StringPrintf(
              "full pkt issue read blob for remianing bytes old offset=%d "
              "len=%d new offset=%d",
              offset, len, p_clcb->counter);
          gatt_act_read(p_clcb, p_clcb->counter);
        } else /* end of request, send callback */
        {
          gatt_end_operation(p_clcb, GATT_SUCCESS, (void*)p_clcb->p_attr_buf);
        }
      } else /* exception, should not happen */
      {
        LOG(ERROR) << "attr offset = " << +offset
                   << " p_attr_buf = " << p_clcb->p_attr_buf;
        gatt_end_operation(p_clcb, GATT_NO_RESOURCES,
                           (void*)p_clcb->p_attr_buf);
      }
    }
  } else {
    if (p_clcb->operation == GATTC_OPTYPE_DISCOVERY &&
        p_clcb->op_subtype == GATT_DISC_INC_SRVC &&
        p_clcb->read_uuid128.wait_for_read_rsp) {
      p_clcb->s_handle = p_clcb->read_uuid128.next_disc_start_hdl;
      p_clcb->read_uuid128.wait_for_read_rsp = false;
      if (len == Uuid::kNumBytes128) {
        p_clcb->read_uuid128.result.value.incl_service.service_type =
            bluetooth::Uuid::From128BitLE(p);
        if (p_clcb->p_reg->app_cb.p_disc_res_cb)
          (*p_clcb->p_reg->app_cb.p_disc_res_cb)(p_clcb->conn_id,
                                                 p_clcb->op_subtype,
                                                 &p_clcb->read_uuid128.result);
        gatt_act_discovery(p_clcb);
      } else {
        gatt_end_operation(p_clcb, GATT_INVALID_PDU, (void*)p);
      }
    }
  }
}

/*******************************************************************************
 *
 * Function         gatt_process_handle_rsp
 *
 * Description      This function is called to handle the write response
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_handle_rsp(tGATT_CLCB* p_clcb) {
  gatt_end_operation(p_clcb, GATT_SUCCESS, NULL);
}
/*******************************************************************************
 *
 * Function         gatt_process_mtu_rsp
 *
 * Description      Process the configure MTU response.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void gatt_process_mtu_rsp(tGATT_TCB& tcb, tGATT_CLCB* p_clcb, uint16_t len,
                          uint8_t* p_data) {
  uint16_t mtu;
  tGATT_STATUS status = GATT_SUCCESS;

  if (len < GATT_MTU_RSP_MIN_LEN) {
    LOG(ERROR) << "invalid MTU response PDU received, discard.";
    status = GATT_INVALID_PDU;
  } else {
    STREAM_TO_UINT16(mtu, p_data);

    if (mtu < tcb.payload_size && mtu >= GATT_DEF_BLE_MTU_SIZE)
      tcb.payload_size = mtu;
  }

  l2cble_set_fixed_channel_tx_data_length(tcb.peer_bda, L2CAP_ATT_CID,
                                          tcb.payload_size);
  gatt_end_operation(p_clcb, status, NULL);
}
/*******************************************************************************
 *
 * Function         gatt_cmd_to_rsp_code
 *
 * Description      Convert an ATT command op code into the corresponding
 *                  response code assume no error occurs.
 *
 * Returns          response code.
 *
 ******************************************************************************/
uint8_t gatt_cmd_to_rsp_code(uint8_t cmd_code) {
  uint8_t rsp_code = 0;

  if (cmd_code > 1 && cmd_code != GATT_CMD_WRITE) {
    rsp_code = cmd_code + 1;
  }
  return rsp_code;
}

/** Find next command in queue and sent to server */
bool gatt_cl_send_next_cmd_inq(tGATT_TCB& tcb) {
  while (!tcb.cl_cmd_q.empty()) {
    tGATT_CMD_Q& cmd = tcb.cl_cmd_q.front();
    if (!cmd.to_send || cmd.p_cmd == NULL) return false;

    tGATT_STATUS att_ret = attp_send_msg_to_l2cap(tcb, cmd.p_cmd);
    if (att_ret != GATT_SUCCESS && att_ret != GATT_CONGESTED) {
      LOG(ERROR) << __func__ << ": L2CAP sent error";
      tcb.cl_cmd_q.pop();
      continue;
    }

    cmd.to_send = false;
    cmd.p_cmd = NULL;

    if (cmd.op_code == GATT_CMD_WRITE || cmd.op_code == GATT_SIGN_CMD_WRITE) {
      /* dequeue the request if is write command or sign write */
      uint8_t rsp_code;
      tGATT_CLCB* p_clcb = gatt_cmd_dequeue(tcb, &rsp_code);

      /* send command complete callback here */
      gatt_end_operation(p_clcb, att_ret, NULL);

      /* if no ack needed, keep sending */
      if (att_ret == GATT_SUCCESS) continue;

      return true;
    }

    gatt_start_rsp_timer(cmd.p_clcb);
    return true;
  }

  return false;
}

/** This function is called to handle the server response to client */
void gatt_client_handle_server_rsp(tGATT_TCB& tcb, uint8_t op_code,
                                   uint16_t len, uint8_t* p_data) {
  if (op_code == GATT_HANDLE_VALUE_IND || op_code == GATT_HANDLE_VALUE_NOTIF) {
    if (len >= tcb.payload_size) {
      LOG(ERROR) << StringPrintf(
          "%s: invalid indicate pkt size: %d, PDU size: %d", __func__, len + 1,
          tcb.payload_size);
      return;
    }

    gatt_process_notification(tcb, op_code, len, p_data);
    return;
  }

  uint8_t cmd_code = 0;
  tGATT_CLCB* p_clcb = gatt_cmd_dequeue(tcb, &cmd_code);
  uint8_t rsp_code = gatt_cmd_to_rsp_code(cmd_code);
  if (!p_clcb || (rsp_code != op_code && op_code != GATT_RSP_ERROR)) {
    LOG(WARNING) << StringPrintf(
        "ATT - Ignore wrong response. Receives (%02x) Request(%02x) Ignored",
        op_code, rsp_code);
    return;
  }

  if (!p_clcb->in_use) {
    LOG(WARNING) << "ATT - clcb already not in use, ignoring response";
    gatt_cl_send_next_cmd_inq(tcb);
    return;
  }

  alarm_cancel(p_clcb->gatt_rsp_timer_ent);
  p_clcb->retry_count = 0;

  VLOG(1) << __func__ << " op_code: " << +op_code << ", len = " << +len
                      << "rsp_code: " << +rsp_code;

  /* the size of the message may not be bigger than the local max PDU size*/
  /* The message has to be smaller than the agreed MTU, len does not count
   * op_code */
  if (len >= tcb.payload_size) {
    LOG(ERROR) << StringPrintf(
        "%s: invalid response pkt size: %d, PDU size: %d", __func__, len + 1,
        tcb.payload_size);
    gatt_end_operation(p_clcb, GATT_ERROR, NULL);
  } else {
    switch (op_code) {
      case GATT_RSP_ERROR:
        gatt_process_error_rsp(tcb, p_clcb, op_code, len, p_data);
        break;

      case GATT_RSP_MTU: /* 2 bytes mtu */
        gatt_process_mtu_rsp(tcb, p_clcb, len, p_data);
        break;

      case GATT_RSP_FIND_INFO:
        gatt_process_read_info_rsp(tcb, p_clcb, op_code, len, p_data);
        break;

      case GATT_RSP_READ_BY_TYPE:
      case GATT_RSP_READ_BY_GRP_TYPE:
        gatt_process_read_by_type_rsp(tcb, p_clcb, op_code, len, p_data);
        break;

      case GATT_RSP_READ:
      case GATT_RSP_READ_BLOB:
      case GATT_RSP_READ_MULTI:
        gatt_process_read_rsp(tcb, p_clcb, op_code, len, p_data);
        break;

      case GATT_RSP_FIND_TYPE_VALUE: /* disc service with UUID */
        gatt_process_find_type_value_rsp(tcb, p_clcb, len, p_data);
        break;

      case GATT_RSP_WRITE:
        if(p_clcb->operation == GATTC_OPTYPE_WRITE)
          gatt_process_handle_rsp(p_clcb);
        break;

      case GATT_RSP_PREPARE_WRITE:
        gatt_process_prep_write_rsp(tcb, p_clcb, op_code, len, p_data);
        break;

      case GATT_RSP_EXEC_WRITE:
        gatt_end_operation(p_clcb, p_clcb->status, NULL);
        break;

      default:
        LOG(ERROR) << __func__ << ": Unknown opcode = " << std::hex << op_code;
        break;
    }
  }

  gatt_cl_send_next_cmd_inq(tcb);
}
