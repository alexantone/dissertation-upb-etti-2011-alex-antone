/*
 * src/singhal.c
 *
 * Singhal's algorithm.
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
static timespec_t sup_syncro;
static uint32 critical_region_simulated_duration = 0;

/*
 * Singhal specifics
 */
enum singhal_msg_types {
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

bool_t Requesting; 	 /* true if the fsm_state is PS_PENDING */
bool_t Executing;    /* true if the fsm_state is PS_EXECUTING */
bool_t My_priority;  /* true if the pending request of peer i has priority over the current incoming request */

typedef bool_t * nodes_set_t;
nodes_set_t Ri = NULL;  /* The requesting set*/
nodes_set_t Ii = NULL;  /* The information set */

uint32 * Ri_val = NULL;

/*
 * Structure of the Singhal DME message
 */
struct singhal_message_s {
    dme_message_hdr_t lm_hdr;
    uint32            type;             /* REQUEST/REPLY */
    uint32            tstamp_sec;
    uint32            tstamp_nsec;
    proc_id_t         pid;              /* even though is redundant it's used to mirror the theory */
} PACKED;
typedef struct singhal_message_s singhal_message_t;

#define SINGHAL_MSG_LEN  (sizeof(singhal_message_t))
#define SINGHAL_DATA_LEN (SINGHAL_MSG_LEN - DME_MESSAGE_HEADER_LEN)

typedef struct request_s {
    uint32 sec_tstamp;
    uint32 nsec_tstamp;
    proc_id_t pid;
} request_t;


static request_t pending_request;
/*
 * Helper functions.
 */

static void print_set_elements(nodes_set_t set, const char * set_name)
{
    char strbuff[256] = {};
    char *px = strbuff;
    int ix = 1;

    while (ix <= nodes_count && (sizeof(strbuff) - (px - strbuff)) > 1) {
        if (set[ix]) {
            px += snprintf(px, sizeof(strbuff) - (px - strbuff) - 1, "%d, ", ix);
        }
        ix++;
    }

    dbg_msg("%s@%p contents : %s", set_name, set, strbuff);
}
#define print_set(S) print_set_elements(S, #S);

static void init_Ri(proc_id_t pid)
{
    if (pid < 1 || pid > nodes_count) {
            return;
    }

    /* Ri is 1 based. Element 0 is ignored */
    memset(Ri, FALSE, nodes_count + 1);
    memset(Ri, TRUE, pid);
    print_set(Ri);
}

static void init_Ii(proc_id_t pid)
{
    if (pid < 1 || pid > nodes_count) {
            return;
    }

    /*
     * Ii is 1 based. Element 0 is ignored.
     * Ii should contain only our pid, but we never use it so we make it void.
     */
    memset(Ii, FALSE, nodes_count + 1);
    print_set(Ii);
}

static inline void add_site_to_set(nodes_set_t set, proc_id_t pid,
                                   const char * set_name)
{
    dbg_msg("Adding to set %s site %llu", set_name, pid);
    if (pid >= 1 && pid <= nodes_count && pid != proc_id) {
        set[pid] = TRUE;
    }
    print_set(set);
}
#define add_to_set(set, site) add_site_to_set(set, site, #set)

static inline void remove_site_from_set(nodes_set_t set, proc_id_t pid,
                                        const char * set_name)
{
    dbg_msg("Removing from set %s site %llu", set_name, pid);
    if (pid >= 1 && pid <= nodes_count) {
        set[pid] = FALSE;
    }
    print_set(set);
}
#define remove_from_set(set, site) remove_site_from_set(set, site, #set)

/*
 * It's very important not to add self to Ri.
 * Asking self for permission is pointless and makes this important test fail
 */
static bool_t void_Ri(void)
{
    print_set(Ri);
	int ix = 1;
	for (; ix <= nodes_count; ix++) {
		if (Ri[ix]) {
			return FALSE;
		}
	}

	return TRUE;
}



/*
 * Give a result similar to strcmp
 */
static int request_prio_cmp(const request_t * const a,
                        const request_t * const b) {
    if ((a->sec_tstamp < b->sec_tstamp) ||
        (a->sec_tstamp == b->sec_tstamp) && (a->nsec_tstamp < b->nsec_tstamp) ||
        (a->sec_tstamp == b->sec_tstamp) && (a->nsec_tstamp == b->nsec_tstamp) && (a->pid < b->pid))
    {
        return -1;
    }
    return 1;
}

static void peer_msg_add_timestamp(singhal_message_t * msg) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    msg->tstamp_sec = htonl((uint32)(ts.tv_sec - sup_syncro.tv_sec));
    msg->tstamp_nsec = htonl((uint32)(ts.tv_nsec - sup_syncro.tv_nsec));
}

/*
 * Prepare a singhal message for network sending.
 */
static int singhal_msg_set(singhal_message_t * const msg, unsigned int msgtype,
                           char * const msctext, size_t msclen)
{
    if (!msg) {
        return ERR_DME_HDR;
    }

    /* first set the header */
    dme_header_set(&msg->lm_hdr, MSGT_SINGHAL, SINGHAL_MSG_LEN, 0);

    /* then the singhal specific data */
    peer_msg_add_timestamp(msg);
    msg->type = htonl(msgtype);
    msg->pid = htonq(proc_id);

    snprintf(msctext, msclen, "%s(ts=%u.%09u, pid=%llu)", msg_type_tostr(msgtype),
             ntohl(msg->tstamp_sec), ntohl(msg->tstamp_nsec), proc_id);

    return 0;
}

/*
 * Parse a received singhal message. The space must be already allocated in 'msg'.
 */
static int singhal_msg_parse(buff_t buff, singhal_message_t * msg) {
    singhal_message_t * src = (singhal_message_t *)buff.data;

    if (!msg || buff.data == NULL || buff.len < DME_MESSAGE_HEADER_LEN) {
        return ERR_DME_HDR;
    }

    /* first parse the header */
    dme_header_parse(buff, &msg->lm_hdr);

    /* then the singhal specific data */
    msg->tstamp_sec = ntohl(src->tstamp_sec);
    msg->tstamp_nsec = ntohl(src->tstamp_nsec);
    msg->type = ntohl(src->type);
    msg->pid = ntohq(src->pid);

}

/*
 * Informs the supervisor that something happened in this porcess's state.
 */
static int supervisor_send_inform_message(dme_ev_t ev) {
    sup_message_t msg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;
    struct timespec tnow;
    uint32 elapsed_sec;
    uint32 elapsed_nsec;

    switch (ev) {
    case DME_EV_ENTERED_CRITICAL_REG:
    case DME_EV_EXITED_CRITICAL_REG:
        clock_gettime(CLOCK_REALTIME, &tnow);
        elapsed_sec = (uint32)(tnow.tv_sec - sup_tstamp.tv_sec);
        elapsed_nsec = (uint32)(tnow.tv_nsec - sup_tstamp.tv_nsec);

        /* construct and send the message */
        sup_msg_set(&msg, ev, elapsed_sec, elapsed_nsec, 0,
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
 * Sends messages only to those sites present in 'set' (except self)
 */
int singhal_set_msg_send (uint8 * buff, size_t len, nodes_set_t set,
                          char * const msctext)
{
    int ix = 0;
    int ret = 0;

    for (ix = 1; ix < proc_id && !ret; ix++) {
    	if (set[ix]) {
    		ret |= dme_send_msg(ix, buff, len, msctext);
    	}
    }

    for (ix = proc_id + 1; ix <= nodes_count && !ret; ix++) {
    	if (set[ix]) {
    		ret |= dme_send_msg(ix, buff, len, msctext);
    	}
    }

    return ret;
}



/*
 * Event handler functions.
 * These functions must properly free the cookie recieved, except for the
 * DME_EV_SUP_MSG_IN and DME_EV_PEER_MSG_IN.
 */

static int fsm_state = PS_IDLE;

static int handle_supervisor_msg(void * cookie) {
    dbg_msg("");
    int ret = 0;
    int ix;
    const buff_t * buff = (buff_t *)cookie;
    sup_message_t srcmsg = {};

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


/* This is the algortihm's implementation */
static int handle_peer_msg(void * cookie) {
    dbg_msg("");
    singhal_message_t srcmsg = {};
    singhal_message_t dstmsg = {};
    char msctext[MAX_MSC_TEXT] = {};
    request_t req = {};
    proc_id_t Sj;
    int ret = 0;
    const buff_t * buff = (buff_t *)cookie;
    int ix;

    if (!buff) {
        dbg_err("Message is empty!");
        return ERR_RECV_MSG;
    }


    /*
     * The Singhal's algorithm uses it's own state variables (Requesting & Executing).
     * The FSM states are only used in supervisor communication.
     * Nevertheless a routine sanity check would prevent the algorithm from
     * advancing in case of state corruption.
     */
    /* FSM sanity check */
    switch(fsm_state) {
    case PS_IDLE:
    case PS_EXECUTING:
    case PS_PENDING:
        break;
    default:
        dbg_err("Fatal error: FSM state corrupted");
        ret = ERR_FATAL;
        return ret;
        break;
    }

    /* Parsing and processing received message */
    singhal_msg_parse(*buff, &srcmsg);
    Sj = srcmsg.pid;

    if (srcmsg.type == MTYPE_REQUEST) {
        /*
         * Emulate the REQUEST message handler
         */

        dbg_msg("Received a REQUEST message from %llu", srcmsg.pid);
        req.sec_tstamp = srcmsg.tstamp_sec;
        req.nsec_tstamp = srcmsg.tstamp_nsec;
        req.pid = srcmsg.pid;

        if (Requesting) {
            My_priority = request_prio_cmp(&req, &pending_request) > 0;

            if (My_priority) {
                dbg_msg("My pending request has priority.");
                add_to_set(Ii, Sj);
            } else {
                /* Send back the REPLY message */
                dbg_msg("Received request has priority. Sending REPLY to %llu", Sj);
                singhal_msg_set(&dstmsg, MTYPE_REPLY, msctext, sizeof(msctext));
                dme_send_msg(Sj, (uint8*)&dstmsg, SINGHAL_MSG_LEN, msctext);

                if(!Ri[Sj]) {
                    dbg_msg("Site %llu was not in Ri. Adding it now.", Sj);
                    add_to_set(Ri, Sj);

                    /*
                     * Send REQ to Sj and update our pending request's time stamp.
                     * (Equivalent to updating the logical lamport clock)
                     */
                    singhal_msg_set(&dstmsg, MTYPE_REQUEST, msctext, sizeof(msctext));
                    pending_request.sec_tstamp = ntohl(dstmsg.tstamp_sec);
                    pending_request.nsec_tstamp = ntohl(dstmsg.tstamp_nsec);

                    dme_send_msg(Sj, (uint8*)&dstmsg, SINGHAL_MSG_LEN, msctext);
                }
            }
        }
        else if (Executing) {
            dbg_msg("Executing CS. Defer reply to %llu (add site to Ii)", Sj);
            add_to_set(Ii, Sj);
        }
        else if (!Executing && !Requesting) {
            /* The condition is not necessary because it's implied by 'else' */
            dbg_msg("We're idle. Add site %llu to Ri and send REPLY.", Sj);
            add_to_set(Ri, Sj);
            /* Send the REPLY message */
            singhal_msg_set(&dstmsg, MTYPE_REPLY, msctext, sizeof(msctext));
            dme_send_msg(Sj, (uint8*)&dstmsg, SINGHAL_MSG_LEN, msctext);
        }

    } else if (srcmsg.type == MTYPE_REPLY) {
        /*
         * Emulate the REPLY message handler
         */
        dbg_msg("Received a REPLY message from %llu", srcmsg.pid);
        remove_from_set(Ri, Sj);

        /* also were're waiting for Ri to become void when we've made a request */
        dbg_msg("");
        dbg_msg("Test if Ri is void");
        if (Requesting && void_Ri()) {
            dbg_msg("[**] Ri became void -> we can enter our CS.");
            ret = handle_event(DME_EV_ENTERED_CRITICAL_REG, NULL);
        }

    } else {
        dbg_err("Protocol error: received an unknown message type (%d).",
                srcmsg.type);
        ret = ERR_RECV_MSG;
    }

    return ret;
}

/*
 * Course of action when requesting the CS.
 */
int process_ev_want_cr(void * cookie)
{
    singhal_message_t dstmsg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;

    dbg_msg("Entered DME_EV_WANT_CRITICAL_REG");

    if (fsm_state != PS_IDLE) {
        dbg_err("Fatal error: DME_EV_WANT_CRITICAL_REG occured while not in IDLE state.");
        return (err = ERR_FATAL);
    }


    /* Switch to the pending state */
    fsm_state = PS_PENDING;

    Requesting = TRUE;

    /* Ask permission from all sites in Ri */
    dbg_msg("");
    dbg_msg("Ask permission from all sites in Ri.");
    print_set(Ri);

    singhal_msg_set(&dstmsg, MTYPE_REQUEST, msctext, sizeof(msctext));

    /* Record my request's time stamp and save o copy of this REQUEST message*/
    pending_request.sec_tstamp = ntohl(dstmsg.tstamp_sec);
    pending_request.nsec_tstamp = ntohl(dstmsg.tstamp_nsec);
    err = singhal_set_msg_send((uint8*)&dstmsg, SINGHAL_MSG_LEN, Ri, msctext);

    /* if Ri is void we can enter the CS directly */
    dbg_msg("Test if Ri is void");
    if (void_Ri()) {
        dbg_msg("[**] Ri is void -> we can enter our CS.");
    	err = handle_event(DME_EV_ENTERED_CRITICAL_REG, NULL);
    } else {
        /* Ri will be checked if it's void when processing REPLY messages */
    }

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

    Requesting = FALSE;
    Executing = TRUE;

    /* Finish our simulated work after the ammount of time specified by the supervisor */
    schedule_event(DME_EV_EXITED_CRITICAL_REG,
                   critical_region_simulated_duration, 0, NULL);

    return err;
}

/*
 * Course of action when leaving the CS.
 */
int process_ev_exited_cr(void * cookie)
{
    singhal_message_t msg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;
    int ix;

    dbg_msg("Entered DME_EV_EXITED_CRITICAL_REG handler");

    if (fsm_state != PS_EXECUTING) {
        dbg_err("Fatal error: DME_EV_EXITED_CRITICAL_REG occured while not in EXECUTING state.");
        return (err = ERR_FATAL);
    }

    /* inform the supervisor */
    supervisor_send_inform_message(DME_EV_EXITED_CRITICAL_REG);
    fsm_state = PS_IDLE;

    Executing = FALSE;

    /* inform all peers from Ii that we left the CS */
    singhal_msg_set(&msg, MTYPE_REPLY, msctext, sizeof(msctext));

    dbg_msg("");
    dbg_msg("Inform all sites in Ii.");
    print_set(Ii);
    err = singhal_set_msg_send((uint8*)&msg, SINGHAL_MSG_LEN, Ii, msctext);

    /* Move sites from Ii to Ri */
    dbg_msg("Move sites from Ii to Ri.");
    for (ix = 1; ix <= nodes_count; ix++) {
    	if (Ii[ix] == TRUE) {
    	    /* commenting function calls to reduce debug messages output */
    		/*
                remove_from_set(Ii, ix);
                add_to_set(Ri, ix);
    		*/

    	    Ii[ix] = FALSE;
    	    Ri[ix] = TRUE;
    	}
    }
    print_set(Ri);
    print_set(Ii);

    return err;
}


int main(int argc, char *argv[])
{
    int res = 0;
    int ix;

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

    /* Create the request set */
    Ri = calloc(nodes_count + 1, sizeof(bool_t));
    Ri_val = calloc(nodes_count + 1, sizeof(uint32));

    /* Create the inform set */
    Ii = calloc(nodes_count + 1, sizeof(bool_t));

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
     * Initializing Singhal specific variables
     */
    init_Ri(proc_id);
    init_Ii(proc_id);

    Requesting = FALSE;
    Executing = FALSE;

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
    safe_free(Ri);
    safe_free(Ii);


    return res;
}
