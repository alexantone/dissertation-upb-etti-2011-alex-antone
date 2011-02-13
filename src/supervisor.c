/*
 * supervisor.c
 *
 * This is the simulated environment supervisor.
 * 
 *  Created on: Dec 3, 2010
 *      Author: alex
 * -------------------------------------------------------------------------
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>


#include <common/defs.h>
#include <common/init.h>
#include <common/util.h>
#include <common/net.h>
#include <common/fsm.h>

/* 
 * global vars, defined in each app.
 * Don't forget to declare them in each ".c" file
 * The supervisor always has proc_id = 0
 */
proc_id_t proc_id = 0;                  /* this process id */
link_info_t * nodes = NULL;             /* peer matrix */
size_t nodes_count = 0;

int err_code = 0;
bool_t exit_request = FALSE;

static char * fname = NULL;
static char * logfname = "supervisor.log";
static FILE * log_fh;


static unsigned int election_interval = 10;     /* time in seconds to rerun election */
static unsigned int concurency_ratio = 50;      /* value in percent of total processes */
static unsigned int critical_sect_time = 5;		/* Duration of the critical section */
static unsigned int max_concurrent_proc = 0;    /* This will be computed in main() */
static bool_t fixed_concurent_num = FALSE;

static unsigned int test_number = 0;			/* The current concurrency test */
static timespec_t tstamp_last_exited;
static timespec_t tstamp_supervisor_start;

static timespec_t * synchro_delays;
static timespec_t * response_times;
static unsigned int elected_proc_count;
static unsigned int received_resps_count;

#define log_msg(format, args...) \
        fprintf(log_fh, format "\n", ##args)

/* 
 * trigger_critical_region()
 * 
 * Trigger request of the critical region for a certain process.
 * Once entered the critical region, that process will stay there
 * for the specified amount of time as sec and nanosec.
 */
static int trigger_critical_region (proc_id_t dest_pid,
                                    uint32 sec_delta, uint32 nsec_delta) {
    sup_message_t msg = {};
    char msctext[MAX_MSC_TEXT] = {};
    uint8 * buff = (uint8 *) &msg;
    
    sup_msg_set(&msg, DME_EV_WANT_CRITICAL_REG, sec_delta, nsec_delta, 0,
                msctext, sizeof(msctext));
    
    return dme_send_msg(dest_pid, buff, SUPERVISOR_MESSAGE_LENGTH, msctext);
}

static void randomizer_init(void) {
	unsigned int seed;
	FILE * fh;

	if (fh = fopen("/dev/urandom","r")) {
		fread(&seed, sizeof(unsigned int), 1, fh);
		fclose(fh);
	} else {
		dbg_msg("NOTICE: Could not open /dev/urandom. The RNG will be affected.");
	}
	srandom(seed);
}

/*
 * Returns a random pid in [1..nodes_count]
 */
static proc_id_t get_random_pid() {
    int ix = 0;
    
    /* get a value in [1 .. nodes_count] */
    ix = 1 + random() % nodes_count;
    return nodes[ix].proc_id;
    
}

/* 
 * Event handler functions.
 * These functions must properly free the cookie received.
 */
int syncro(void * cookie) {
    sup_message_t msg = {};
    char msctext[MAX_MSC_TEXT] = {};
    uint8 * buff = (uint8 *) &msg;
    timespec_t ts_syncro;
    timespec_t *pts = NULL;

    if ((timespec_t*)cookie == NULL) {
        clock_gettime(CLOCK_REALTIME, &ts_syncro);
        pts = &ts_syncro;
    } else {
        pts = (timespec_t*)cookie;
    }

    sup_msg_set(&msg, DME_SEV_SYNCRO,
                (uint32)pts->tv_sec, (uint32)pts->tv_nsec, 0,
                msctext, sizeof(msctext));

    return dme_broadcast_msg(buff, SUPERVISOR_MESSAGE_LENGTH, msctext);

}

int do_work(void * cookie) {
	dbg_msg("=================================================================");
    int concurrent_count = fixed_concurent_num ?
            max_concurrent_proc : random() % (max_concurrent_proc - 1) + 2;
    proc_id_t pid_arr[concurrent_count];
    proc_id_t tpid;
    bool_t found;
    int ix;
    int jx;
    char strbuff[256];
    char * px = strbuff;
    timespec_t avg_resp_time;
    timespec_t avg_synchro_time;
    
    /* If the critical region is free, elect processes to compete for it */
    if (critical_region_is_idle() && concurrent_count > 0) {
    	/*
    	 * First collect statistics from last test run
    	 */
    	if (elected_proc_count > 0 && received_resps_count > 0) {
    		dbg_msg("Collecting statistics for test run %d", test_number);
    		if (received_resps_count < elected_proc_count) {
    			dbg_msg("Not all processes finished processing their CS!!! (%u/%u)",
    					received_resps_count, elected_proc_count);
    		}


    		avg_synchro_time = timespec_avg(synchro_delays, received_resps_count);
    		avg_resp_time = timespec_avg(response_times, received_resps_count);

    		dbg_msg("Test %2d: procs=%d avg_synchro_time=%ld.%09lu avg_resp_time=%ld.%09lu",
    				test_number, elected_proc_count,
    				avg_synchro_time.tv_sec, avg_synchro_time.tv_nsec,
    				avg_resp_time.tv_sec, avg_resp_time.tv_nsec);
    		log_msg("Test %2d: procs=%d avg_synchro_time=%ld.%09lu avg_resp_time=%ld.%09lu",
    				test_number, elected_proc_count,
    				avg_synchro_time.tv_sec, avg_synchro_time.tv_nsec,
    				avg_resp_time.tv_sec, avg_resp_time.tv_nsec);

    		/* List the synchronization delays */
    		memset(strbuff, 0, sizeof(strbuff));
    		px = strbuff;
    		for (ix = 0; ix < elected_proc_count; ix++) {
    		px += snprintf(px, sizeof(strbuff) - (px - strbuff) - 1, "%3ld.%09lu, ",
    				synchro_delays[ix].tv_sec, synchro_delays[ix].tv_nsec);
    		}
    		dbg_msg("SYNCRO DELAYS:  %s", strbuff);
    		log_msg("SYNCRO DELAYS:  %s", strbuff);


    		/* List the response times */
    		memset(strbuff, 0, sizeof(strbuff));
    		px = strbuff;
    		for (ix = 0; ix < elected_proc_count; ix++) {
    		px += snprintf(px, sizeof(strbuff) - (px - strbuff) - 1, "%3ld.%09lu, ",
    				response_times[ix].tv_sec, response_times[ix].tv_nsec);
    		}
    		dbg_msg("RESPONSE TIMES: %s", strbuff);
    		log_msg("RESPONSE TIMES: %s", strbuff);

    	} else {
    		dbg_msg("Ignoring test run %d (%u of %u responses)",
    				test_number, received_resps_count, elected_proc_count);
    		log_msg("Ignoring test run %d (%u of %u responses)",
    				test_number, received_resps_count, elected_proc_count);
    	}
    	fflush(log_fh);

    	/*
    	 * Prepare the test
    	 */
    	elected_proc_count = concurrent_count;
    	received_resps_count = 0;
    	memset(synchro_delays, 0, nodes_count * sizeof(synchro_delays[0]));
    	memset(response_times, 0, nodes_count * sizeof(response_times[0]));

        clock_gettime(CLOCK_REALTIME, &tstamp_last_exited);
    	test_number++;

    	/* Start the test */
    	dbg_msg("Critical region is free. Starting election process for %d processes.",
    			concurrent_count);
        /* build the list of competing processes */
        ix = 0;
        while (ix < concurrent_count) {
            tpid = get_random_pid();
            
            /* Avoid duplicates */
            found = FALSE;
            for(jx = 0; jx < ix && !found; jx++) {
                if (pid_arr[jx] == tpid) {
                    found = TRUE;
                }
            }
            
            /* 
             * This process id was not elected before. Add it to the list.
             * Else replay the loop.
             */
            if (!found) {
                pid_arr[ix] = tpid;
                dbg_msg("Elected process %llu", tpid);
                ix++;
            }
        }
        
        /* Trigger the elected processes to compete for the critical region */
        for (ix = 0; ix < concurrent_count; ix++) {
            trigger_critical_region(pid_arr[ix], critical_sect_time, 0);
            nodes[pid_arr[ix]].state = PS_PENDING;
        }
    } else {
    	dbg_msg("Critical region is not free yet. Rescheduling.");
    }
    
    
    /* reschedule this process */
    schedule_event(DME_SEV_PERIODIC_WORK, election_interval, 0, NULL);
}

/* Process incoming messages */
int process_messages(void * cookie)
{
    dbg_msg("");
    sup_message_t srcmsg = {};
    int err = 0;
    timespec_t tnow;
    timespec_t tdelta;
    timespec_t tprogdelta;
    
    if (err = sup_msg_parse(*(buff_t *)cookie, &srcmsg)) {
        return err;
    }
    
    clock_gettime(CLOCK_REALTIME, &tnow);
    tprogdelta = timespec_delta(tstamp_supervisor_start, tnow);

    switch(srcmsg.msg_type) {
    case DME_EV_ENTERED_CRITICAL_REG:
        nodes[srcmsg.process_id].state = PS_EXECUTING;
        dbg_msg("[%ld.%09lu] ENTERED CS: process %llu waited for %u.%09u seconds to enter the CS",
        		tprogdelta.tv_sec, tprogdelta.tv_nsec,
        		srcmsg.process_id, srcmsg.sec_tdelta, srcmsg.nsec_tdelta);

        /* Get synchronization delay */
        tdelta = timespec_delta(tstamp_last_exited, tnow);
        dbg_msg("SYNCHRONIZATION DELAY is %ld.%09lu", tdelta.tv_sec, tdelta.tv_nsec);
        synchro_delays[received_resps_count].tv_sec = tdelta.tv_sec;
        synchro_delays[received_resps_count].tv_nsec = tdelta.tv_nsec;

        /* Get the response time */
        response_times[received_resps_count].tv_sec = srcmsg.sec_tdelta;
        response_times[received_resps_count].tv_nsec = srcmsg.nsec_tdelta;

        /* Advance the responses counter */
        received_resps_count++;
        dbg_msg("received_resps_count = %u", received_resps_count);

        if (!critical_region_is_sane()) {
            dbg_err("Unfortunately there are multiple processes in the CS at the same time!");
            err = ERR_FATAL;
        }
        break;
        
    case DME_EV_EXITED_CRITICAL_REG:
        nodes[srcmsg.process_id].state = PS_IDLE;

        /* mark the time */
        tstamp_last_exited.tv_sec = tnow.tv_sec;
        tstamp_last_exited.tv_nsec = tnow.tv_nsec;

        dbg_msg("[%ld.%09lu] EXITED CS: process %llu stayed for %u.%09u seconds in it's CS",
        		tprogdelta.tv_sec, tprogdelta.tv_nsec,
                srcmsg.process_id, srcmsg.sec_tdelta, srcmsg.nsec_tdelta);
        break;

    default:
        /* Other types are invalid */
        break;
    }
    
    return err;
}

int main(int argc, char *argv[])
{
    int res = 0;
    
    if (0 != (res = parse_sup_params(argc, argv, &fname, &logfname,
                                     &concurency_ratio, &max_concurrent_proc,
                                     &election_interval))) {
        dbg_err("parse_args() returned nonzero status:%d", res);
        goto end;
    }

    /*
     * Parse the file fname
     */
    if (0 != (res = parse_file(fname, proc_id, &nodes, &nodes_count))) {
        dbg_err("parse_file() returned nonzero status:%d", res);
        goto end;
    }
    dbg_msg("nodes has %d elements", nodes_count);

    /* compute the number of maximum concurrent processes (nearest integer) */
    if (max_concurrent_proc != 0) {
        /* The '-c' option was specified on the command line */
        fixed_concurent_num = TRUE;
    } else {
        /* Compute based on concurrency ratio */
        max_concurrent_proc = (nodes_count * concurency_ratio + 50) / 100;
        fixed_concurent_num = FALSE;
    }

    if (max_concurrent_proc < 2) {
        dbg_err("concurrency ratio or number set too low. At least 2 processes must be concurrent.");
        goto end;
    } else if (max_concurrent_proc > nodes_count) {
        dbg_err("concurrency number is greater than total number of processes. Setting it to maximum.");
        max_concurrent_proc = nodes_count;
    }

    dbg_msg("max_concurrent_proc=%d", max_concurrent_proc);

    randomizer_init();
    
    /* Allocate the statistics collection storage and open the log file */
    synchro_delays = calloc(nodes_count, sizeof(timespec_t));
    response_times = calloc(nodes_count, sizeof(timespec_t));

    if (NULL == (log_fh = fopen(logfname, "w"))) {
        dbg_err("Could not open log file %s for writing", logfname);
        res = ERR_BADFILE;
        goto end;
    }


    /*
     * Init connections (open listening socket)
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
    
    register_event_handler(DME_SEV_PERIODIC_WORK, do_work);
    register_event_handler(DME_SEV_SYNCRO, syncro);
    register_event_handler(DME_SEV_MSG_IN, process_messages);

    /* wait for peers processes to init, then kick start the supervisor */
    sleep(5);
    
    /* Start working; mark time */
    clock_gettime(CLOCK_REALTIME, &tstamp_supervisor_start);

    handle_event(DME_SEV_SYNCRO, &tstamp_supervisor_start);
    sleep(1);

    handle_event(DME_SEV_PERIODIC_WORK, NULL);

    /*
     * Main loop: just sit here and wait for interrupts.
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

    if (log_fh) {
        fclose(log_fh);
    }

    safe_free(nodes);
    safe_free(synchro_delays);
    safe_free(response_times);
    
    return res;
}
