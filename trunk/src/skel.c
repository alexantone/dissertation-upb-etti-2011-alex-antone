/*
 * /dme/src/skel.c
 *
 * Generic process skeleton
 *
 *  Created on: Dec 11, 2010
 *      Author: alex
 * -------------------------------------------------------------------------
 */

/* Base system includes */
#include <stdio.h>
#include <unistd.h>
#include <time.h>

/* DME API includes */
#include <common/defs.h>
#include <common/init.h>
#include <common/fsm.h>
#include <common/util.h>
#include <common/net.h>

/*
 * global vars, defined in each app
 * Don't forget to declare them in each ".c" file
 */
proc_id_t proc_id = 0;                  /* this process id */
link_info_t * nodes = NULL;             /* peer matrix */
size_t nodes_count = 0;                 /* number of nodes (sites) */
static char * fname = NULL;

/* Program flow vars */
int err_code = 0;
bool_t exit_request = FALSE;

/* Other vars */
static struct timespec sup_tstamp;      /* used for performance measurements */
static uint32 critical_region_simulated_duration = 5;

/*
 * Algorithm Specifics
 */

/* Message types used by the algorithm */
enum generic_msg_types {
    MTYPE_REQUEST,
    MTYPE_REPLY,
    MTYPE_RELEASE,
};

static inline
const char * msg_type_tostr(int mtype) {
    switch(mtype) {
    case MTYPE_REQUEST: return "REQUEST";
    case MTYPE_REPLY:   return "REPLY";
    case MTYPE_RELEASE: return "RELEASE";
    }
    return "UNKNOWN";
}


/*
 * Generic alg. message structure.
 * Every alg. should include the common header.
 */
struct generic_message_s {
    /* common header */
    dme_message_hdr_t lm_hdr;

    /* Algorithm specific fields */
    uint32 field1;
    uint8  field2[20];
} PACKED;
typedef struct generic_message_s generic_message_t;

#define GENERIC_MSG_LEN  (sizeof(generic_message_t))
#define GENERIC_DATA_LEN (GENERIC_MSG_LEN - DME_MESSAGE_HEADER_LEN)

/*
 * Other structures used by the algorithm (queues/arrays/trees etc.)
 */

/* struct <generic> ... */

/*
 * Helper functions (porcess specific structures,parse messages etc.).
 */

/* func1 ... */
/* func2 ... */

/*
 * Prepare a generic message for network sending.
 */
static int generic_msg_set(generic_message_t * const msg, unsigned int msgtype,
                           char * const msctext, size_t msclen)
{
    if (!msg) {
        return ERR_DME_HDR;
    }

    /*
     * First set the header.
     * The Message type must be added to the list in common/net.h
     */

    /* dme_header_set(&msg->lm_hdr, MSGT_GENERIC, GENERIC_MSG_LEN, 0); */

    /* then the generic alg. specific data which must be converted to network order*/

    /* msg->field1 = hton..(...) */
    snprintf(msctext, msclen, "%s( )", msg_type_tostr(msgtype));

    return 0;
}

/*
 * Parse a received generic message. The space must be already allocated in 'msg'.
 */
static int generic_msg_parse(buff_t buff, generic_message_t * msg) {
    generic_message_t * src = (generic_message_t *)buff.data;

    if (!msg || buff.data == NULL || buff.len < DME_MESSAGE_HEADER_LEN) {
        return ERR_DME_HDR;
    }

    /* first parse the header */
    dme_header_parse(buff, &msg->lm_hdr);

    /* then the generic alg. specific data which must be converted from network order */

    /* msg->field1 = ntoh..(....) */

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
        err = dme_send_msg(SUPERVISOR_PID, (uint8*)&msg, SUPERVISOR_MESSAGE_LENGTH,
                           msctext);

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
static timespec_t sup_syncro;

static int handle_supervisor_msg(void * cookie) {
    const buff_t * buff = (buff_t *)cookie;
    sup_message_t srcmsg = {};
    int ret = 0;

    if (!buff) {
        dbg_err("Message is empty!");
        return ERR_RECV_MSG;
    }

    switch(fsm_state) {
    case PS_IDLE:
        /* record the time */
        clock_gettime(CLOCK_REALTIME, &sup_tstamp);
        sup_msg_parse(*buff, &srcmsg);

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


/*
 * This is the algortihm's implementation
 * Note: the FSM state transition should be done with handle_event(ev, cookie)
 *       and whould be the last operation in the logical flow of the function.
 *       This in needed to assure that no other message processing is handled
 *       between the state change decision and the actual state change.
 */
static int handle_peer_msg(void * cookie) {
    generic_message_t srcmsg = {};
    generic_message_t dstmsg = {};
    int ret = 0;
    const buff_t * buff = (buff_t *)cookie;
    int ix;

    if (!buff) {
        dbg_err("Message is empty!");
        return ERR_RECV_MSG;
    }

    /* parse the received buffer in the srcmsg structure */
    generic_msg_parse(*buff, &srcmsg);

    switch(fsm_state) {
    case PS_IDLE:
        /* process peer message*/
        break;

    case PS_EXECUTING:
        /* process peer message*/
        break;

    case PS_PENDING:
        /* process peer message*/
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
    generic_message_t dstmsg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;

    dbg_msg("Entered DME_EV_WANT_CRITICAL_REG");

    if (fsm_state != PS_IDLE) {
        dbg_err("Fatal error: DME_EV_WANT_CRITICAL_REG occured while not in IDLE state.");
        return (err = ERR_FATAL);
    }


    /* Switch to the pending state and send informs to peers */
    fsm_state = PS_PENDING;

    /* Do whatever IPC is necessary */
    generic_msg_set(&dstmsg, MTYPE_REQUEST, msctext, sizeof(msctext));
    err = dme_broadcast_msg((uint8*)&dstmsg, GENERIC_MSG_LEN, msctext);

    return err;
}


/*
 * Course of action when entering the CS.
 */
int process_ev_entered_cr(void * cookie)
{
    int err = 0;

    dbg_msg("Entered DME_EV_ENTERED_CRITICAL_REG");

    if (fsm_state != PS_PENDING) {
        dbg_err("Fatal error: DME_EV_ENTERED_CRITICAL_REG occured while not in PENDING state.");
        return (err = ERR_FATAL);
    }

    /* Switch to the executing state and inform the supervisor */
    supervisor_send_inform_message(DME_EV_ENTERED_CRITICAL_REG);
    fsm_state = PS_EXECUTING;

    /* Finish our simulated work after the amount of time specified by the supervisor */
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
    generic_message_t msg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;

    dbg_msg("Entered DME_EV_EXITED_CRITICAL_REG");

    if (fsm_state != PS_EXECUTING) {
        dbg_err("Fatal error: DME_EV_EXITED_CRITICAL_REG occured while not in EXECUTING state.");
        return (err = ERR_FATAL);
    }

    /* inform the supervisor */
    supervisor_send_inform_message(DME_EV_EXITED_CRITICAL_REG);

    /* Switch state to IDLE */
    fsm_state = PS_IDLE;

    /* Do whatever IPC deems necessary */
    generic_msg_set(&msg, MTYPE_RELEASE, msctext, sizeof(msctext));
    err = dme_broadcast_msg((uint8*)&msg, GENERIC_MSG_LEN, msctext);

    return err;
}


int main(int argc, char *argv[])
{
    int res = 0;

    /* Parse command line parameters */
    if (0 != (res = parse_peer_params(argc, argv, &proc_id, &fname))) {
        dbg_err("parse_args() returned nonzero status:%d", res);
        goto end;
    }

    /*
     * Parse the DME simulation config file.
     */
    if (0 != (res = parse_file(fname, proc_id, &nodes, &nodes_count))) {
        dbg_err("parse_file() returned nonzero status:%d", res);
        goto end;
    }

    /* Initialize and allocate any structures/vars used by the generic algorithm */

    /* struct genericstruct = calloc(...) */

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

    /* Register event handlers that will be called automatically on event occurence */
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

    /* Free allocated structures */
    safe_free(nodes);

    return res;
}

