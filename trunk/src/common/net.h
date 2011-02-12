/*
 * src/common/net.h
 *
 * Network related functions.
 * 
 *  Created on: Nov 14, 2010
 *      Author: alex
 * -------------------------------------------------------------------------
 */

#ifndef NET_H_
#define NET_H_

#include <common/defs.h>

/* hton() & ntoh() for 64 bit values */
static inline uint64 htonq(uint64 q) {
    return ((htonl(q >> 32)) | ((uint64)htonl((uint32) q) << 32));
}
#define ntohq(q) htonq(q)
#define MAX_MSC_TEXT 256

extern int dme_send_msg(proc_id_t dest, uint8 * buff, size_t len, char * const msctext);
extern int dme_recv_msg(uint8 ** out_buff, size_t * out_len);

extern int dme_broadcast_msg(uint8 * buff, size_t len, char * const msctext);

/* Message types for each algorithm */
typedef enum msg_types_e {
    MSGT_LAMPORT,
    MSGT_SUZUKI,
    MSGT_SINGHAL,
    MSGT_RICART,
} msg_type_t;

/* 
 * The DME message format
 * Flags are not defined for now.
 */
/*
 *   0                8                 16                24                32 
 *   +- - - - - - - - + - - - - - - - - + - - - - - - - - + - - - - - - - - +
 * 0 |                           Packet MAGIC                               |
 *   |----------------------------------------------------------------------|
 * 1 |                             Process ID                               |
 * 2 |                              (64 bits)                               |
 *   |----------------------------------------------------------------------|
 * 3 |         Message Type             |            Flags                  |
 *   |----------------------------------------------------------------------|
 * 4 |         Length                   | xxxxxxxxxxx Reserved xxxxxxxxxxx  |
 *   |----------------------------------------------------------------------|
 * 5 |                                                                      |
 * . |                                ....                                  |
 * . |                      DATA  (aligned to 4B)                           |
 * . |                                ....                                  |
 *   |                                                                      |
 * 
 */

#define DME_MSG_MAGIC (0xDAAEAA59)  /* DMEMSG in 31137 speech :) (AA -> M) */
struct dme_message_hdr_s {
    uint32      dme_magic;              /* DME magic checksum */
    uint64      process_id;
    uint16      msg_type;               /* defines the type of the algorithm */
    uint16      flags;
    uint16      length;                 /* length of the following data */
    
    /* A structure specific for each algorithm will start from here */
    uint8       data[0]; 
} PACKED;
typedef struct dme_message_hdr_s dme_message_hdr_t;

#define DME_MESSAGE_HEADER_LEN (sizeof(dme_message_hdr_t))



/* The superisor messages format. It has fixed length */
/*
 *   0                8                 16                24                32 
 *   |- - - - - - - - + - - - - - - - - + - - - - - - - - + - - - - - - - - |
 * 0 |                           Packet MAGIC                               |
 *   |----------------------------------------------------------------------|
 * 1 |                             Process ID                               |
 * 2 |                              (64 bits)                               |
 *   |----------------------------------------------------------------------|
 * 3 |      Event  (Message Type)       |            Flags                  |
 *   |----------------------------------------------------------------------|
 * 4 |                            Time delta secs.                          |
 *   |----------------------------------------------------------------------|
 * 5 |                         Time delta nanosecs.                         |
 *   +----------------------------------------------------------------------+
 * 
 */

#define SUP_MSG_MAGIC (0x500FAA59)  /* SUPMSG (SOOFMSg) in 31137 speech :) */
struct sup_message_s {
    uint32      sup_magic;      /* supervisor magic checksum */
    uint64      process_id;     
    uint16      msg_type;       /* The event triggered/occured */
    uint16      flags;
    uint32      sec_tdelta;
    uint32      nsec_tdelta;
} PACKED;
typedef struct sup_message_s sup_message_t;

#define SUPERVISOR_MESSAGE_LENGTH (sizeof(struct sup_message_s))


extern int dme_header_set(dme_message_hdr_t * const hdr, unsigned int msgtype,
                          unsigned int msglen, unsigned int flags);

extern int sup_msg_set(sup_message_t * const msg, unsigned int msgtype,
                       uint32 sec_delta, uint32 nsec_delta, unsigned int flags,
                       char * const mscbuf, size_t msclen);

extern int dme_header_parse(buff_t buff, dme_message_hdr_t * const msg);
extern int sup_msg_parse(buff_t buff, sup_message_t * const msg);

#endif /* NET_H_ */
