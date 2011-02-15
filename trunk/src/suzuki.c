/*
 * src/suzuki.c
 *
 * Suzuki's algorithm.
 *
 *  Created on: Dec 9, 2010
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
static uint32 critical_region_simulated_duration = 0;

/*
 * Suzuki specifics
 */
enum suzuki_msg_types {
    MTYPE_REQUEST,
    MTYPE_REPLY,
};

const char * msg_type_tostr(int mtype) {
    switch(mtype) {
    case MTYPE_REQUEST: return "REQUEST";
    case MTYPE_REPLY:   return "REPLY";
    }
    return "UNKNOWN";
}

unsigned int suzuki_RN[50] = {0}; //RN[j] is the largest order number received so far


/*
 * Structure of the suzuki DME message
 */

struct token_s{						/*token structure*/
	unsigned int suzuki_LN[50];
	unsigned int pseudo_queue[50];
};

struct suzuki_message_s {
	dme_message_hdr_t lm_hdr;
    uint32            type;             /* REQUEST/REPLY/RELEASE */
    proc_id_t         pid;              /* even though is redundant it's used to mirror the theory */
    unsigned int 	  req_no;		/*request number*/
    struct token_s 	  token; 		/*token */
} PACKED;

bool_t i_have_token = FALSE;

struct token_s my_token;

typedef struct suzuki_message_s suzuki_message_t;

#define SUZUKI_MSG_LEN  (sizeof(suzuki_message_t))
#define SUZUKI_DATA_LEN (SUZUKI_MSG_LEN - DME_MESSAGE_HEADER_LEN)

/*
 * Debugging functions
 */
static char * token_tostr(struct token_s *tok, char * const buf, size_t len)
{
    size_t pos = 0;
    int ix;

    for (ix = 0 ; ix < sizeof(tok->pseudo_queue) && tok->pseudo_queue[ix]; ix++) {
        pos += snprintf(buf + pos, len - pos, "%u, ", tok->pseudo_queue[ix]);
    }
    buf[pos] = '\0';

    return buf;
}

/*
 * Helper functions.
 */
static int request_queue_final_idx() {
	int ix = 0;
	while ( my_token.pseudo_queue[ix] != 0 ){
		ix++;
	}

	return (ix == 0 ? -1 : ix - 1);
}


static bool_t is_in_queue(proc_id_t pid){
	int ix = 0;

	while (my_token.pseudo_queue[ix] != 0 && my_token.pseudo_queue[ix] != pid ) {
		ix++;
	}

	return (my_token.pseudo_queue[ix] == pid);
}

static void request_queue_insert(unsigned int const p_pid) {
    /* if queue is empty just create the queue */

    int final_element_in_queue = request_queue_final_idx();
    int i = 0;

    final_element_in_queue = final_element_in_queue < 0 ? 0 : final_element_in_queue;

	dbg_msg("QUEUE: current top pid is %d", my_token.pseudo_queue[ final_element_in_queue ]);
	//now we have the final element
	for (i = final_element_in_queue + 1; i >= 1 ; i-- ){
		my_token.pseudo_queue[ i ] = my_token.pseudo_queue [ i-1 ];
	}
	//the pseudo queue is shifted by one element
	my_token.pseudo_queue[ 0 ] = p_pid;
	if (final_element_in_queue)
		dbg_msg("QUEUE: new top pid is %d", my_token.pseudo_queue[ final_element_in_queue + 1 ]);
	else
		dbg_msg("QUEUE: new top pid is %d", my_token.pseudo_queue[ final_element_in_queue ]);
    return;
}

static void request_queue_pop(void){
    int final_element_in_queue = request_queue_final_idx();
    int i = 0;

    final_element_in_queue = final_element_in_queue < 0 ? 0 : final_element_in_queue;

    dbg_msg("QUEUE: current top pid is %d", my_token.pseudo_queue[ final_element_in_queue ]);
    my_token.pseudo_queue[ final_element_in_queue ] = 0;
    if (final_element_in_queue)
		dbg_msg("QUEUE: new top pid is %d", my_token.pseudo_queue[ final_element_in_queue - 1 ]);
	else
		dbg_msg("QUEUE: new top pid is %d", my_token.pseudo_queue[ final_element_in_queue ]);
}

/*
 * Prepare a suzuki message for network sending.
 */
static int suzuki_msg_set(suzuki_message_t * const msg, unsigned int msgtype,
                          char * const msctext, size_t msclen)
{
    char tokbuf[256] = {};
    if (!msg) {
        return ERR_DME_HDR;
    }

    /* first set the header */
    dme_header_set(&msg->lm_hdr, MSGT_SUZUKI, SUZUKI_MSG_LEN, 0);

    /* then the suzuki specific data */

    msg->type = htonl(msgtype);
    msg->pid = htonq(proc_id);
    msg->req_no = htonl(suzuki_RN[proc_id]);
    msg->token = my_token;


    snprintf(msctext, msclen, "%s(pid=%llu, reqno=%u,tok: {%s})",
             msg_type_tostr(msgtype), proc_id, suzuki_RN[proc_id],
             token_tostr(&my_token, tokbuf, sizeof(tokbuf)));

    return 0;
}

/*
 * Parse a received suzuki message. The space must be already allocated in 'msg'.
 */
static int suzuki_msg_parse(buff_t buff, suzuki_message_t * msg) {
    suzuki_message_t * src = (suzuki_message_t *)buff.data;

    if (!msg || buff.data == NULL || buff.len < DME_MESSAGE_HEADER_LEN) {
        return ERR_DME_HDR;
    }

    /* first parse the header */
    dme_header_parse(buff, &msg->lm_hdr);

    /* then the suzuki specific data */
    msg->type = ntohl(src->type);
    msg->pid = ntohq(src->pid);
    msg->req_no = ntohl(src->req_no);
    msg->token = src->token;
    return 0;
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
    dbg_msg("");
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
    	/* eu */
    	dbg_msg("Ignoring message from supervisor (not in IDLE state).");
    	break; /**/
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
    proc_id_t dst_pid;
    suzuki_message_t srcmsg = {};
    suzuki_message_t dstmsg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int ret = 0;
    const buff_t * buff = (buff_t *)cookie;
    int ix;

    if (!buff) {
        dbg_err("Message is empty!");
        return ERR_RECV_MSG;
    }

    suzuki_msg_parse(*buff, &srcmsg);
    dbg_msg("Recieved a %s from peer %llu (currently holding token=%d)",
    		srcmsg.type == MTYPE_REPLY ? "REPLY" : "REQUEST", srcmsg.pid, i_have_token);
    switch(fsm_state) {
    case PS_IDLE:
    	if ( srcmsg.type == MTYPE_REQUEST)
    		if (i_have_token == TRUE){

    			if ( suzuki_RN[srcmsg.pid] < srcmsg.req_no ){
    				suzuki_RN[srcmsg.pid] = srcmsg.req_no;
    			}
    			//bag in coada mesajul
    			if (suzuki_RN[srcmsg.pid] == (my_token.suzuki_LN[srcmsg.pid] + 1) ){
    				if ( is_in_queue(srcmsg.pid) == FALSE )
    					request_queue_insert(srcmsg.pid);
    			}

    			int final_element_in_queue = request_queue_final_idx();

				dst_pid = my_token.pseudo_queue[final_element_in_queue];
				request_queue_pop();
				suzuki_msg_set(&dstmsg, MTYPE_REPLY, msctext, sizeof(msctext));
				dme_send_msg(dst_pid, (uint8*)&dstmsg, SUZUKI_MSG_LEN, msctext);
				i_have_token = FALSE;
				memset(&my_token , 0 , sizeof(my_token));
    		}else {
    			if ( suzuki_RN[srcmsg.pid] < srcmsg.req_no ){
    				suzuki_RN[srcmsg.pid] = srcmsg.req_no;
    			}
    		}

    	break;

    case PS_EXECUTING:
    	if ( srcmsg.type == MTYPE_REQUEST)
    		if ( suzuki_RN[srcmsg.pid] < srcmsg.req_no ){
    			suzuki_RN[srcmsg.pid] = srcmsg.req_no;
    		}
    	break;

    case PS_PENDING:
        if (srcmsg.type == MTYPE_REQUEST) {
            dbg_msg("Recieved a REQUEST message");
            if ( suzuki_RN[srcmsg.pid] < srcmsg.req_no ){
            	suzuki_RN[srcmsg.pid] = srcmsg.req_no;
			}
        } else if (srcmsg.type == MTYPE_REPLY){
            dbg_msg("Received a REPLY message");
            i_have_token = TRUE;
            my_token = srcmsg.token;
            //start executing
            ret = handle_event(DME_EV_ENTERED_CRITICAL_REG, NULL);
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
int process_ev_want_cr()
{
    suzuki_message_t dstmsg = {};
    char msctext[MAX_MSC_TEXT] = {};
    int err = 0;

    dbg_msg("Entered DME_EV_WANT_CRITICAL_REG");

    if (fsm_state != PS_IDLE) {
        dbg_err("Fatal error: DME_EV_WANT_CRITICAL_REG occured while not in IDLE state.");
        return (err = ERR_FATAL);
    }


    /* Switch to the pending state and send informs to peers */
    fsm_state = PS_PENDING;

    suzuki_RN[proc_id]++;
    if (i_have_token == FALSE){
		suzuki_msg_set(&dstmsg, MTYPE_REQUEST, msctext, sizeof(msctext));
		err = dme_broadcast_msg((uint8*)&dstmsg, SUZUKI_MSG_LEN, msctext);
    } else {
    	queue_event(DME_EV_ENTERED_CRITICAL_REG, NULL);
    }
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

    dbg_msg("Exitting");
    return err;
}

/*
 * Course of action when leaving the CS.
 */
int process_ev_exited_cr(void * cookie)
{
    suzuki_message_t dstmsg = {};
    char msctext[MAX_MSC_TEXT] = {};
    proc_id_t dst_pid;
    int err = 0;
    int ix;
    int final_element_in_queue = -1;
    dbg_msg("");

    if (fsm_state != PS_EXECUTING) {
        dbg_err("Fatal error: DMEis_in_queue_EV_EXITED_CRITICAL_REG occured while not in EXECUTING state.");
        return (err = ERR_FATAL);
    }

    /* inform the supervisor */
    supervisor_send_inform_message(DME_EV_EXITED_CRITICAL_REG);
    my_token.suzuki_LN[proc_id]++;
    fsm_state = PS_IDLE;

	for (ix=1; ix<50; ix++){
		if (suzuki_RN[ix] == (my_token.suzuki_LN[ix] + 1) ){
			if ( is_in_queue(ix) == FALSE ){
				request_queue_insert(ix);
			}
		}
	}

	final_element_in_queue = request_queue_final_idx();
	if (final_element_in_queue >= 0) {
		dst_pid = my_token.pseudo_queue[final_element_in_queue];
		request_queue_pop();
		suzuki_msg_set(&dstmsg, MTYPE_REPLY, msctext, sizeof(msctext));
		dme_send_msg(dst_pid, (uint8*)&dstmsg, SUZUKI_MSG_LEN, msctext);
		i_have_token = FALSE;
		memset(&my_token , 0 , sizeof(my_token));
	} else {
		dbg_msg("INFO: No other pending processes.");
	}

    return err;
}


int main(int argc, char *argv[])
{
    int res = 0;

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

    memset(suzuki_RN, 0, sizeof(suzuki_RN));
    memset(&my_token, 0, sizeof(my_token));

    /*
     * Main loop: just sit here and wait for interrupts (triggered by the supervisor).
     * All work is done in interrupt handlers mapped to registered functions.
     */
    struct token_s token;

    if ( proc_id == 1 ){
    	i_have_token = TRUE;
    }

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

    return res;
}
