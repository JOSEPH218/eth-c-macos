/*
 * RNDIS-over-USB protocol constants and wire structures.
 *
 * Reference: Microsoft "Remote NDIS (RNDIS) Specification", and the
 * behavior of the Linux g_ether/u_ether "rndis" USB gadget function,
 * which is what the Samsung/Android side of the cable actually runs.
 */
#ifndef ETHC_RNDIS_H
#define ETHC_RNDIS_H

#include <stdint.h>

/* USB class-specific control requests used to carry RNDIS messages
 * (CDC "encapsulated command" mechanism, USB CDC spec 6.2.13/6.2.14). */
#define RNDIS_REQ_SEND_ENCAPSULATED_COMMAND  0x00
#define RNDIS_REQ_GET_ENCAPSULATED_RESPONSE  0x01

/* Notification sent on the control interface's interrupt IN endpoint
 * telling the host a response is ready to be pulled with
 * GET_ENCAPSULATED_RESPONSE. */
#define RNDIS_NOTIFY_RESPONSE_AVAILABLE      0x01

/* RNDIS message types (MessageType field, little-endian on the wire). */
#define RNDIS_MSG_PACKET            0x00000001u
#define RNDIS_MSG_INITIALIZE        0x00000002u
#define RNDIS_MSG_INITIALIZE_CMPLT  0x80000002u
#define RNDIS_MSG_HALT              0x00000003u
#define RNDIS_MSG_QUERY             0x00000004u
#define RNDIS_MSG_QUERY_CMPLT       0x80000004u
#define RNDIS_MSG_SET               0x00000005u
#define RNDIS_MSG_SET_CMPLT         0x80000005u
#define RNDIS_MSG_RESET             0x00000006u
#define RNDIS_MSG_RESET_CMPLT       0x80000006u
#define RNDIS_MSG_INDICATE_STATUS   0x00000007u
#define RNDIS_MSG_KEEPALIVE         0x00000008u
#define RNDIS_MSG_KEEPALIVE_CMPLT   0x80000008u

#define RNDIS_STATUS_SUCCESS  0x00000000u

#define RNDIS_MAJOR_VERSION  1
#define RNDIS_MINOR_VERSION  0

/* Object IDs we need. */
#define OID_GEN_SUPPORTED_LIST         0x00010101u
#define OID_GEN_CURRENT_PACKET_FILTER  0x0001010Eu
#define OID_GEN_MAXIMUM_FRAME_SIZE     0x00010106u
#define OID_802_3_PERMANENT_ADDRESS    0x01010101u
#define OID_802_3_CURRENT_ADDRESS      0x01010102u

#define NDIS_PACKET_TYPE_DIRECTED    0x00000001u
#define NDIS_PACKET_TYPE_MULTICAST   0x00000002u
#define NDIS_PACKET_TYPE_BROADCAST   0x00000004u
#define NDIS_PACKET_TYPE_ALL_MULTICAST 0x00000010u

/* Generous upper bound: control responses and per-packet data both fit
 * well under this on every RNDIS gadget we've seen. */
#define RNDIS_CONTROL_BUFFER_SIZE  1024
#define RNDIS_MAX_FRAME_SIZE       1600

#pragma pack(push, 1)

struct rndis_msg_header {
    uint32_t message_type;
    uint32_t message_length;
};

struct rndis_initialize_msg {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t request_id;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t max_transfer_size;
};

struct rndis_initialize_cmplt {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t request_id;
    uint32_t status;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t device_flags;
    uint32_t medium;
    uint32_t max_packets_per_transfer;
    uint32_t max_transfer_size;
    uint32_t packet_alignment_factor;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct rndis_query_msg {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_buffer_length;
    uint32_t info_buffer_offset;
    uint32_t reserved;
};

struct rndis_query_cmplt {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t request_id;
    uint32_t status;
    uint32_t info_buffer_length;
    uint32_t info_buffer_offset;
};

struct rndis_set_msg {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_buffer_length;
    uint32_t info_buffer_offset;
    uint32_t reserved;
};

struct rndis_set_cmplt {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t request_id;
    uint32_t status;
};

struct rndis_keepalive_msg {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t request_id;
};

struct rndis_keepalive_cmplt {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t request_id;
    uint32_t status;
};

/* Data path framing: every Ethernet frame sent/received over the bulk
 * endpoints is wrapped in one of these. data_offset is measured from
 * the start of the data_offset field itself (RNDIS spec quirk). */
struct rndis_packet_msg {
    uint32_t message_type;
    uint32_t message_length;
    uint32_t data_offset;
    uint32_t data_length;
    uint32_t oob_data_offset;
    uint32_t oob_data_length;
    uint32_t num_oob_data_elements;
    uint32_t per_packet_info_offset;
    uint32_t per_packet_info_length;
    uint32_t vc_handle;
    uint32_t reserved;
};

/* Per RNDIS spec, DataOffset is measured from the start of the
 * DataOffset field itself to the start of the data (which immediately
 * follows this fixed header). */
#define RNDIS_PACKET_MSG_DATA_OFFSET \
    (sizeof(struct rndis_packet_msg) - offsetof(struct rndis_packet_msg, data_offset))

/* InformationBufferOffset in QUERY/SET messages (both request and
 * completion) is measured from the start of the RequestID field. */
#define RNDIS_INFO_BUFFER_OFFSET(struct_type) \
    (sizeof(struct_type) - offsetof(struct_type, request_id))

#pragma pack(pop)

#endif /* ETHC_RNDIS_H */
