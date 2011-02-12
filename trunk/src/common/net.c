/*
 * src/common/net.c
 *
 * Network related functions.
 * 
 *  Created on: Nov 14, 2010
 *      Author: alex
 * -------------------------------------------------------------------------
 */


#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

#include <sys/socket.h>
#include <common/net.h>
#include <common/init.h>

/* Global variables from main process */
extern const link_info_t * const nodes;
extern const size_t nodes_count;
extern const proc_id_t proc_id;

#define MSC_SEP '|'

static int msc_msg(proc_id_t srcid, proc_id_t dstid, char * const msctext) {
    struct timespec ts;
    const char * px = NULL;
    const char * sx = msctext;
    char linebuf[MAX_MSC_TEXT];
    int linesize;
    int linecnt = 0;

    clock_gettime(CLOCK_REALTIME, &ts);

    bzero(linebuf, sizeof(linebuf));
    px = strchr(sx, MSC_SEP);

    if (px == NULL) {
        /* Print to the end of the string */
        snprintf(linebuf, sizeof(linebuf), "%s", sx);
    } else {
        /* Print to next line */
        snprintf(linebuf, px - sx,  "%s", sx);
    }

    fprintf(stdout, "%09u.%09u %02d # p%llu->p%llu: %s\n",
            (uint32)ts.tv_sec, (uint32)ts.tv_nsec, linecnt,
            srcid, dstid, linebuf);
    linecnt++;

    /* Print other MSC commands */
    while (px != NULL) {
        bzero(linebuf, sizeof(linebuf));
        sx = ++px;
        px = strchr(sx, MSC_SEP);

        if (0 == strncmp(sx, "activate_src", 14)) {
            fprintf(stdout, "%09u.%09u %02d # activate p%llu\n",
                    (uint32)ts.tv_sec, (uint32)ts.tv_nsec, linecnt,
                    srcid);
        }
        else if (0 == strncmp(sx, "activate_dst", 14)) {
            fprintf(stdout, "%09u.%09u %02d # activate p%llu\n",
                    (uint32)ts.tv_sec, (uint32)ts.tv_nsec, linecnt,
                    dstid);
        }
        else if (0 == strncmp(sx, "deactivate_src", 16)) {
            fprintf(stdout, "%09u.%09u %02d # deactivate p%llu\n",
                    (uint32)ts.tv_sec, (uint32)ts.tv_nsec, linecnt,
                    srcid);
        }
        else if (0 == strncmp(sx, "deactivate_src", 16)) {
            fprintf(stdout, "%09u.%09u %02d # deactivate p%llu\n",
                    (uint32)ts.tv_sec, (uint32)ts.tv_nsec, linecnt,
                    dstid);
        }

        linecnt++;
    }

    fflush(stdout);
    return 0;
}


/*
 * Send the buffer to node with process_id dest.
 */
int dme_send_msg(proc_id_t dest, uint8 * buff, size_t len, char * const msctext)
{
    dbg_msg("send_msg(dest=%llu, buff@%p, len=%u)", dest, buff, len);
    int maxcount = nodes_count;
    struct sockaddr * dest_addr = NULL;
    
    if (dest < 0 || dest > maxcount) {
        dbg_err("Destination process id is out of bounds: %llu not in [0..%d]",
                dest, maxcount);
        return ERR_SEND_MSG;
    }
    
    dest_addr = (struct sockaddr *)&nodes[dest].listen_addr;
    
    msc_msg(proc_id, dest, msctext);
    sendto(nodes[proc_id].sock_fd, buff, len, 0, dest_addr, sizeof(*dest_addr));
    return 0;
}

/*
 * Send a message to all the other nodes (except self):
 * {1, .., nodes_count} \ { proc_id }
 */
int dme_broadcast_msg (uint8 * buff, size_t len, char * const msctext) {
    int ix = 0;
    int ret = 0;
    
    for (ix = 1; ix < proc_id && !ret; ix++) {
        ret |= dme_send_msg(ix, buff, len, msctext);
    }
    for (ix = proc_id + 1; ix <= nodes_count && !ret; ix++) {
        ret |= dme_send_msg(ix, buff, len, msctext);
    }
    
    return ret;
}


/* 
 * Recieve a message and return an allocated buffer and length.
 * The buffer MUST BE DEALLOCATED in the calling function!
 */

#define MAX_PACK_LEN    (1024) /* To avoid fragmentation -> UDP fails */
static uint8 test_buff[MAX_PACK_LEN];

int dme_recv_msg(uint8 ** out_buff, size_t * out_len)
{
    int len = 0;
    *out_len = 0; /* initialize to 0 just to avoid reading an empty buffer */
    
    /* Determine the length of the packet first */
    len = recv(nodes[proc_id].sock_fd, test_buff, MAX_PACK_LEN, MSG_PEEK);
    
    if (len <= 0 || !(*out_buff = malloc(len))) {
        dbg_err("Could not allocate buffer of length %d", len);
        return ERR_RECV_MSG;
    }
    
    /* Recieve the real data */
    if (len != recv(nodes[proc_id].sock_fd, *out_buff, len, 0)) {
        dbg_err("The expected packet length has changed! How did this happen??");
        safe_free(*out_buff);
        return ERR_RECV_MSG;
    }
    
    /* Now it's safe to report the retrieved buffer length */
    *out_len = len;
    return 0;
}

/*
 * Prepare a DME message header for network sending.
 */
int dme_header_set(dme_message_hdr_t * const hdr, unsigned int msgtype,
                   unsigned int msglen, unsigned int flags)
{
    if (!hdr) {
        return ERR_DME_HDR;
    }

    hdr->dme_magic = htonl(DME_MSG_MAGIC);
    hdr->process_id = htonq(proc_id);
    hdr->msg_type = htons((uint16)msgtype);
    hdr->length = htons((uint16)msglen);
    hdr->flags = htons((uint16)flags);;
    
    return 0;
}

/*
 * Prepare a SUP message for network sending.
 */
int sup_msg_set(sup_message_t * const msg, unsigned int msgtype,
                uint32 sec_delta, uint32 nsec_delta, unsigned int flags,
                char * const mscbuf, size_t msclen)
{
    char * msccmd = "";
    if (!msg) {
        return ERR_SUP_HDR;
    }

    msg->sup_magic = htonl(SUP_MSG_MAGIC);
    msg->process_id = htonq(proc_id);
    msg->msg_type = htons((uint16)msgtype);
    msg->sec_tdelta = htonl(sec_delta);
    msg->nsec_tdelta = htonl(nsec_delta);
    msg->flags = htonl((uint16)flags);
    
    if (msgtype == DME_EV_ENTERED_CRITICAL_REG) {
        msccmd = "activate_src";
    } else if (msgtype == DME_EV_EXITED_CRITICAL_REG) {
        msccmd = "deactivate_src";
    }

    snprintf(mscbuf, msclen, "%s(%u.%09u) %c%s",
             evtostr(msgtype), sec_delta, nsec_delta,
             MSC_SEP, msccmd);

    return 0;
}

/*
 * Parse a recieved DME message. The space must be allready allocated in 'hdr'.
 */
int dme_header_parse(buff_t buff, dme_message_hdr_t * const msg)
{
    dme_message_hdr_t * src = (dme_message_hdr_t *)buff.data;

    if (!msg || buff.data == NULL || buff.len < DME_MESSAGE_HEADER_LEN) {
        return ERR_DME_HDR;
    }

    msg->dme_magic = ntohl(src->dme_magic);
    msg->process_id = ntohq(src->process_id);
    msg->msg_type = ntohs(src->msg_type);
    msg->length = ntohs(src->length);
    msg->flags = ntohs(src->flags);;
    
    return 0;
}

/*
 * Parse a recieved SUP message. The space must be allready allocated in 'msg'.
 */
int sup_msg_parse(buff_t buff, sup_message_t * const msg)
{
    sup_message_t * src = (sup_message_t *)buff.data;

    if (!msg || buff.data == NULL || buff.len < SUPERVISOR_MESSAGE_LENGTH) {
        return ERR_SUP_HDR;
    }

    msg->sup_magic = ntohl(src->sup_magic);
    msg->process_id = ntohq(src->process_id);
    msg->msg_type = ntohs(src->msg_type);
    msg->sec_tdelta = ntohl(src->sec_tdelta);
    msg->nsec_tdelta = ntohl(src->nsec_tdelta);
    msg->flags = ntohl(src->flags);
    
    return 0;
}



