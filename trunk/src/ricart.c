/*
 * src/ricart.c
 *
 * Ricart-Agrawala algorithm.
 *
 *  Created on: Nov 6, 2010
 *      Author: alex
 * -------------------------------------------------------------------------
 */

#include <stdio.h>
#include <unistd.h>
#include <time.h>


#include "common/defs.h"
#include "common/init.h"
#include "common/fsm.h"
#include "common/util.h"
#include "common/net.h"

/*
 * global vars, defined in each app
 * Don't forget to declare them in each ".c" file
 */
proc_id_t proc_id = 0;                  /* this process id */
link_info_t * nodes = NULL;             /* peer matrix */
size_t nodes_count = 0;

int err_code = 0;
bool_t exit_request = FALSE;

static char * fname = NULL;
static struct timespec sup_tstamp;      /* used for performance measurements */
static timespec_t sup_syncro;           /* used to sync with the supervisor */
static uint32 critical_region_simulated_duration = 0;

/*
 * Ricart specifics
 */
enum ricart_msg_types {
    MTYPE_REQUEST,
    MTYPE_REPLY,
};

static inline
const char * msg_type_tostr(int mtype) {
    switch(mtype) {
    case MTYPE_REQUEST: return "REQUEST";
    case MTYPE_REPLY:   return "REPLY";
    }
    return "UNKNOWN";
}

static int *ricart_RD = NULL;
static bool_t *ricart_replies = NULL; 

/*
 * Structure of the ricart DME message
 */
struct ricart_message_s {
    dme_message_hdr_t lm_hdr;
    uint32            type;             /* REQUEST/REPLY/RELEASE */
    uint32            tstamp_sec;
    uint32            tstamp_nsec;
    proc_id_t         pid;              /* even though is redundant it's used to mirror the theory */
} PACKED;

static uint32 my_tstamp_sec;
static uint32 my_tstamp_nsec;
 

typedef struct ricart_message_s ricart_message_t;

#define RICART_MSG_LEN  (sizeof(ricart_message_t))
#define RICART_DATA_LEN (RICART_MSG_LEN - DME_MESSAGE_HEADER_LEN)


/*
 * Checks if it's this processes turn to enter the CS
 */
static bool_t my_turn(void) {
    int ix;
    bool_t keep_going = TRUE;
    /* First check if all the replies arrived from the other peers */
    for (ix = 1; ix < proc_id && keep_going; ix++) {
        keep_going = keep_going && ricart_replies[ix];
    }

    if (!keep_going) {
    	return FALSE;
    }
    dbg_msg("Passed first part");

    for (ix = proc_id + 1; ix <= nodes_count && keep_going; ix++) {
        keep_going = keep_going && ricart_replies[ix];
    }

    if (!keep_going) {
    	return FALSE;
    }
    dbg_msg("Passed second part");

    /* If all peers replied*/
    return keep_going;
}

static void peer_msg_add_timestamp(ricart_message_t * msg) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    msg->tstamp_sec = htonl((uint32)(ts.tv_sec - sup_syncro.tv_sec));
    msg->tstamp_nsec = htonl((uint32)(ts.tv_nsec - sup_syncro.tv_nsec));
}

/*
 * Prepare a ricart message for network sending.
 */
static int ricart_msg_set(ricart_message_t * const msg, unsigned int msgtype,
                          char * const msctext, size_t msclen)
{
    if (!msg) {
        return ERR_DME_HDR;
    }

    /* first set the header */
    dme_header_set(&msg->lm_hdr, MSGT_RICART, RICART_MSG_LEN, 0);

    /* then the ricart specific data */
    peer_msg_add_timestamp(msg);
    msg->type = htonl(msgtype);
    msg->pid = htonq(proc_id);

    snprintf(msctext, msclen, "%s(%u.%09u, %llu)", msg_type_tostr(msgtype),
             ntohl(msg->tstamp_sec), ntohl(msg->tstamp_nsec), proc_id);

    return 0;
}

/*
 * Parse a received ricart message. The space must be already allocated in 'msg'.
 */
static int ricart_msg_parse(buff_t buff, ricart_message_t * msg) {
    ricart_message_t * src = (ricart_message_t *)buff.data;

    if (!msg || buff.data == NULL || buff.len < DME_MESSAGE_HEADER_LEN) {
        return ERR_DME_HDR;
    }

    /* first parse the header */
    dme_header_parse(buff, &msg->lm_hdr);
    /* then the ricart specific data */
    msg->tstamp_sec = ntohl(src->tstamp_sec);
    msg->tstamp_nsec = ntohl(src->tstamp_nsec);
    msg->type = ntohl(src->type);
    msg->pid = ntohq(src->pid);
    dbg_msg("msg parse from %llu , type = %u ",msg->pid, msg->type);
    dbg_msg("src msg parse timestamp  = %u sec %u nsec", msg->tstamp_sec , msg->tstamp_nsec);
}

/*
 * Informs the supervisor that something happened in this porcess's state.
 */
static int supervisor_send_inform_message(dme_ev_t ev) {
    sup_message_t msg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;
    timespec_t tnow;
    timespec_t tdelta;

    switch (ev) {
    case DME_EV_ENTERED_CRITICAL_REG:
    case DME_EV_EXITED_CRITICAL_REG:
        clock_gettime(CLOCK_REALTIME, &tnow);
        tdelta = timespec_delta(sup_tstamp, tnow);

        /* construct and send the message */
        sup_msg_set(&msg, ev, tdelta.tv_sec, tdelta.tv_nsec, 0,
                    msctext, sizeof(msctext));
        err = dme_send_msg(SUPERVISOR_PID, (uint8*)&msg, SUPERVISOR_MESSAGE_LENGTH, msctext);

        /* set new sup_tstamp to tnow */
        sup_tstamp.tv_sec = tnow.tv_sec;
        sup_tstamp.tv_nsec = tnow.tv_nsec;
        break;

    default:
        /* No need to send informs to the supervisor in other cases */
        break;
    }

    return err;
}


/*
 * Event handler functions.
 * These functions must properly free the cookie recieved, except fo the
 * DME_EV_SUP_MSG_IN and DME_EV_PEER_MSG_IN.
 */

static int fsm_state = PS_IDLE;

static int handle_supervisor_msg(void * cookie) {
    dbg_msg("Entry point");
    int ret = 0;
    int ix;
    const buff_t * buff = (buff_t *)cookie;
    sup_message_t srcmsg = {};

    if (!buff) {
        dbg_err("Message is empty!");
        return ERR_RECV_MSG;
    }

    /* record the time */
    clock_gettime(CLOCK_REALTIME, &sup_tstamp);
    ret = sup_msg_parse(*buff, &srcmsg);

    if (srcmsg.msg_type == DME_SEV_ENDSIMULATION) {
        exit_request = TRUE;
        return ret;
    }

    switch(fsm_state) {
    case PS_IDLE:

        if (srcmsg.msg_type == DME_SEV_SYNCRO) {
            sup_syncro.tv_sec = srcmsg.sec_tdelta;
            sup_syncro.tv_nsec = srcmsg.nsec_tdelta;
        }
        else if (srcmsg.msg_type == DME_EV_WANT_CRITICAL_REG) {
            critical_region_simulated_duration = srcmsg.sec_tdelta;
            ret = handle_event(DME_EV_WANT_CRITICAL_REG, NULL);
        }

        break;

    /* The supervisor should not send a message in these states */
    case PS_PENDING:
    case PS_EXECUTING:
        dbg_msg("Ignoring message from supervisor (not in IDLE state).");
        break;
    default:
        dbg_err("Fatal error: FSM state corrupted");
        ret = ERR_FATAL;
        break;
    }

    return ret;
}


/* This is the algortihm's implementation */
static int handle_peer_msg(void * cookie) {
    dbg_msg("Entry point");
    char msctext[MAX_MSC_TEXT] = {};
    ricart_message_t srcmsg = {};
    ricart_message_t dstmsg = {};
    int ret = 0;
    const buff_t * buff = (buff_t *)cookie;
    int ix;

    if (!buff) {
        dbg_err("Message is empty!");
        return ERR_RECV_MSG;
    }

    ricart_msg_parse(*buff, &srcmsg);

    switch(fsm_state) {
    case PS_IDLE:
        if (srcmsg.type == MTYPE_REQUEST){
            dbg_msg("Recieved a REQUEST message from %llu", srcmsg.pid);
            /* Send back the REPLY message */
            ricart_RD[(unsigned int)srcmsg.pid] = 0;
            ricart_msg_set(&dstmsg, MTYPE_REPLY, msctext, sizeof(msctext));
            dme_send_msg(srcmsg.pid, (uint8*)&dstmsg, RICART_MSG_LEN, msctext);
        }
        break;
    case PS_EXECUTING:
        if (srcmsg.type == MTYPE_REQUEST){
            dbg_msg("Recieved a REQUEST message from %llu", srcmsg.pid);
            ricart_RD[(unsigned int)srcmsg.pid] = 1;
        }
        break;
    case PS_PENDING:
        if (srcmsg.type == MTYPE_REQUEST) {
            dbg_msg("Recieved a REQUEST message from %llu", srcmsg.pid);

            dbg_msg("my timestamp  = %u sec %u nsec", my_tstamp_sec , my_tstamp_nsec);
            dbg_msg("src timestamp = %u sec %u nsec", srcmsg.tstamp_sec, srcmsg.tstamp_nsec);
            if ( srcmsg.tstamp_sec > my_tstamp_sec){
                ricart_RD[(unsigned int)srcmsg.pid] = 1;
            }else if ( srcmsg.tstamp_sec < my_tstamp_sec){
                ricart_msg_set(&dstmsg, MTYPE_REPLY, msctext, sizeof(msctext));
                dme_send_msg(srcmsg.pid, (uint8*)&dstmsg, RICART_MSG_LEN, msctext);
                dbg_msg("sending REPLY msg to %llu\n",srcmsg.pid);
            }else if ( srcmsg.tstamp_nsec > my_tstamp_nsec){
                ricart_RD[(unsigned int)srcmsg.pid] = 1;
            }else if ( srcmsg.tstamp_nsec < my_tstamp_nsec){
                ricart_msg_set(&dstmsg, MTYPE_REPLY, msctext, sizeof(msctext));
                dme_send_msg(srcmsg.pid, (uint8*)&dstmsg, RICART_MSG_LEN, msctext);
                dbg_msg("sending REPLY msg to %llu\n",srcmsg.pid);
            }
        }else  if (srcmsg.type == MTYPE_REPLY) {
             /* We're waiting for replies from all other peers */
            dbg_msg("Recieved a REPLY message from %llu", srcmsg.pid);
            ricart_replies[srcmsg.pid] = TRUE;
            for (ix = 1 ; ix <= nodes_count; ix++) {
                dbg_msg("ricart_replies[%d] = %s" , ix, ricart_replies[ix] ? "TRUE" : "FALSE");
            }
            /* check if this process can run now */
            if (my_turn()) {
                dbg_msg("My turn now!!!");
                ret = handle_event(DME_EV_ENTERED_CRITICAL_REG, NULL);
            }
        }
        break;

    default:
        dbg_err("Fatal error: FSM state corrupted");
        ret = ERR_FATAL;
        break;
    }

    return ret;
}

/*
 * Course of action when requesting the CS.
 */
int process_ev_want_cr(void * cookie)
{
    ricart_message_t dstmsg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;

    dbg_msg("Entered DME_EV_WANT_CRITICAL_REG");

    if (fsm_state != PS_IDLE) {
        dbg_err("Fatal error: DME_EV_WANT_CRITICAL_REG occured while not in IDLE state.");
        return (err = ERR_FATAL);
    }

    /* Switch to the pending state and send informs to peers */
    fsm_state = PS_PENDING;
    
    /* Clear the table of REPLY messages from peers (1 based) */
    memset(ricart_replies, FALSE, nodes_count * sizeof(bool_t) + 1);
    
    ricart_msg_set(&dstmsg, MTYPE_REQUEST, msctext, sizeof(msctext));
    my_tstamp_sec = ntohl(dstmsg.tstamp_sec);
    my_tstamp_nsec = ntohl(dstmsg.tstamp_nsec);
    dbg_msg("my timestamp  = %u sec %u nsec", my_tstamp_sec , my_tstamp_nsec);
  
    err = dme_broadcast_msg((uint8*)&dstmsg, RICART_MSG_LEN, msctext);
    
    return err;
}


/*
 * Course of action when entering the CS.
 */
int process_ev_entered_cr(void * cookie)
{
    dbg_msg("");
    int err = 0;

    dbg_msg("Entered DME_EV_ENTERED_CRITICAL_REG");

    if (fsm_state != PS_PENDING) {
        dbg_err("Fatal error: DME_EV_ENTERED_CRITICAL_REG occured while not in PENDING state.");
        return (err = ERR_FATAL);
    }

    /* Switch to the executing state and inform the supervisor */
    supervisor_send_inform_message(DME_EV_ENTERED_CRITICAL_REG);
    fsm_state = PS_EXECUTING;

    /* Finish our simulated work after the ammount of time specified by the supervisor */
    schedule_event(DME_EV_EXITED_CRITICAL_REG,
                   critical_region_simulated_duration, 0, NULL);

    dbg_msg("Exit point");
    return err;
}

/*
 * Course of action when leaving the CS.
 */
int process_ev_exited_cr(void * cookie)
{
    ricart_message_t msg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;
    dbg_msg("Entry point");

    if (fsm_state != PS_EXECUTING) {
        dbg_err("Fatal error: DME_EV_EXITED_CRITICAL_REG occured while not in EXECUTING state.");
        return (err = ERR_FATAL);
    }

    /* inform the supervisor */
    supervisor_send_inform_message(DME_EV_EXITED_CRITICAL_REG);
    int ix = 0; 
    ricart_message_t dstmsg;
    for (ix = 1; ix <=nodes_count ; ix++){
        if (ricart_RD[ix] == 1){
            ricart_RD[ix] = 0;
            ricart_msg_set(&dstmsg, MTYPE_REPLY, msctext, sizeof(msctext));
            dme_send_msg(ix, (uint8*)&dstmsg, RICART_MSG_LEN, msctext);
        }
    }
    fsm_state = PS_IDLE;
    return err;
}


int main(int argc, char *argv[])
{
    int res = 0;
    int ix = 0;
    if (0 != (res = parse_peer_params(argc, argv, &proc_id, &fname))) {
        dbg_err("parse_args() returned nonzero status:%d", res);
        goto end;
    }

    /*
     * Parse the file in fname
     */
    if (0 != (res = parse_file(fname, proc_id, &nodes, &nodes_count))) {
        dbg_err("parse_file() returned nonzero status:%d", res);
        goto end;
    }
    dbg_msg("nodes has %d elements", nodes_count);

    /* Create the reply status array (1 based) */
    ricart_replies = calloc(nodes_count + 1, sizeof(bool_t));
    ricart_RD = calloc(nodes_count + 1, sizeof(int));

    /*
     * Init connections (open listenning socket)
     */
    if (0 != (res = open_listen_socket(proc_id, nodes, nodes_count))) {
        dbg_err("open_listen_socket() returned nonzero status:%d", res);
        goto end;
    }

    /*
     * Register signals (for I/O, alarms, etc.)
     */
    if (0 != (res = init_handlers(nodes[proc_id].sock_fd))) {
        dbg_err("init_handlers() returned nonzero status");
        goto end;
    }

    register_event_handler(DME_EV_SUP_MSG_IN, handle_supervisor_msg);
    register_event_handler(DME_EV_PEER_MSG_IN, handle_peer_msg);
    register_event_handler(DME_EV_WANT_CRITICAL_REG, process_ev_want_cr);
    register_event_handler(DME_EV_ENTERED_CRITICAL_REG, process_ev_entered_cr);
    register_event_handler(DME_EV_EXITED_CRITICAL_REG, process_ev_exited_cr);

    /*
     * Main loop: just sit here and wait for interrupts (triggered by the supervisor).
     * All work is done in interrupt handlers mapped to registered functions.
     */
    wait_events();

end:
    /*
     * Do cleanup (deallocating dynamic structures)
     */
    deinit_handlers();

    /* Close our listening socket */
    if (nodes && nodes[proc_id].sock_fd > 0) {
        close(nodes[proc_id].sock_fd);
    }

    safe_free(nodes);
    safe_free(ricart_replies);
    safe_free(ricart_RD);

    return res;
}
