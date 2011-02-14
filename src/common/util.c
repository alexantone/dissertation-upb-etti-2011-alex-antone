/*
 * src/common/util.c
 *
 * Common system routines used by all processes.
 * 
 *  Created on: Nov 23, 2010
 *      Author: alex
 * 
 */

#include <stdio.h>
#include <arpa/inet.h>
#include <common/util.h>
#include <unistd.h>

/*
 * getopt() defines and lib variables
 */
extern char *optarg;
extern int optind, opterr, optopt;


#define PEER_USAGE_MESSAGE \
"Usage:\n"\
"       <algorithm-name>  -i <process-id> -f <config-file>\n"

#define PEER_OPT_STRING "i:f:"
int parse_peer_params(int argc, char ** argv,
                      proc_id_t *out_proc_id, char ** out_fname)
{
    char optchar = '\0';
    bool_t file_provided = FALSE;
    bool_t procid_provided = FALSE;
    
    if (!out_proc_id || !out_fname) {
        return 1;
    }

    while ((optchar = getopt(argc, argv, PEER_OPT_STRING)) != -1) {
        switch(optchar) {
        case 'i':
            *out_proc_id = strtoull(optarg, NULL, BASE_10);
            procid_provided = TRUE;
            break;

        case 'f':
            *out_fname = optarg;
            file_provided = TRUE;
            break;

        default:
            /* Print usage */
            fprintf(stdout, PEER_USAGE_MESSAGE);
            exit(ERR_BADARGS);
            break;
        }
    }
    
    if (!(procid_provided && file_provided)) {
            /* Print usage */
            fprintf(stdout, PEER_USAGE_MESSAGE);
            exit(ERR_BADARGS);
    }
    
    dbg_msg("proc_id=%llu file=%s", *out_proc_id, *out_fname);
    
    return 0;
}

#define SUPERVISOR_USAGE_MESSAGE \
"Usage:\n"\
"       supervisor -f <config-file> [-r <concurency ratio>] [-c <cproc_count>]\n"\
"                  [-t <sec interval>] [-s <number of runs>]\n"\
"                  [-o <out-logfile>]\n"\
" Note: concurrent proc. count takes precedence over the the concurency ratio."


#define SUPERVISOR_OPT_STRING "f:t:r:c:o:s:"
extern int parse_sup_params(int argc, char * argv[],
                            char ** out_fname,
                            char ** out_logfname,
                            uint32 *out_concurency_ratio,
                            uint32 *out_concurent_count,
                            uint32 *out_election_interval,
                            uint32 *out_number_of_runs)
{
    char optchar = '\0';
    bool_t file_provided = FALSE;
    int testval;
    bool_t err = FALSE;
    
    if (!out_fname || !out_concurency_ratio || !out_election_interval) {
        return 1;
    }

    while ((optchar = getopt(argc, argv, SUPERVISOR_OPT_STRING)) != -1) {
        switch(optchar) {
        case 'f':
            *out_fname = optarg;
            file_provided = TRUE;
            break;

        case 'o':
            *out_logfname = optarg;
            break;

        case 'r':
            testval = strtoul(optarg, NULL, BASE_10);
            if (testval < 0 || testval > 100) {
                fprintf(stderr, "Concurrency ratio must be in percent: (0..100).\n");
                err = TRUE;
            } else {
                *out_concurency_ratio = testval;
            }
            break;

        case 'c':
            testval = strtoul(optarg, NULL, BASE_10);
            if (testval < 2) {
                fprintf(stderr, "Concurrent process count must be at least 2.\n");
                err = TRUE;
            } else {
                *out_concurent_count = testval;
            }
            break;

        case 't':
            testval = strtoul(optarg, NULL, BASE_10);
            if (testval < 5 || testval > 300) {
                fprintf(stderr, "Election period must be in (5..300).\n");
                err = TRUE;
            } else {
                *out_election_interval = testval;
            }
            break;

        case 's':
            testval = strtoul(optarg, NULL, BASE_10);
            if (testval < 1) {
                fprintf(stderr, "Number of runs must be at least 1.\n");
                err = TRUE;
            } else {
                *out_number_of_runs = testval;
            }
            break;

        default:
            /* Print usage */
            fprintf(stdout, SUPERVISOR_USAGE_MESSAGE);
            exit(ERR_BADARGS);
            break;
        }
    }
    
    if (!file_provided || err) {
            /* Print usage */
            fprintf(stdout, SUPERVISOR_USAGE_MESSAGE);
            exit(ERR_BADARGS);
    }
    
    if (strcmp(*out_logfname, *out_fname) == 0) {
        dbg_err("The output log file can not be the same as the input file!");
        exit(ERR_BADARGS);
    }

    return 0;
}



#define TOK_DELIM " ;:\t\r\n"
static inline uint64 speed_mult(char prefix)
{
    switch(prefix) {
    case 'k':
    case 'K':
        return KBITPS;
    case 'm':
    case 'M':
        return MBITPS;
    case 'g':
    case 'G':
        return GBITPS;
    default:
        /* This ins not valid so we don't want to interpret it */
        return 1;
    }
    return 1;
}

int parse_file(const char * fname, proc_id_t p_id,
               link_info_t ** out_nodes, size_t * out_nodes_count)
{
    FILE *fh = NULL;
    int prc_count = 0;
    int ix = 0;
    int jx = 0;
    char linebuf[256];
    char *tok;
    char *mult;
    
    uint64 lnk_speed;
        
    link_info_t * cnode = NULL;
    
    
    if (NULL == (fh = fopen(fname, "r"))) {
        dbg_err("Could not open file %s", fname);
        return ERR_BADFILE;
    }
    
    fscanf(fh, "%d", &prc_count);
    dbg_msg("prc_count = %d + 1 supervisor (proc_id = 0)", prc_count);
    fgets(linebuf, sizeof(linebuf), fh);
    
    dbg_msg("Trying to allocate nodes[%d] of size %d * %d",
            prc_count, prc_count, sizeof(link_info_t));
    if (!(*out_nodes = (link_info_t *)calloc(prc_count + 1, sizeof(link_info_t)))) {
        dbg_err("Could not allocate the nodes matrix");
        fclose(fh);
        return ERR_MALLOC;
    }
    *out_nodes_count = prc_count;
    dbg_msg("Allocated nodes array nodes[%d]", *out_nodes_count);
    
    ix = 0;
    while (ix <= prc_count && fgets(linebuf, sizeof(linebuf), fh) != NULL) {
        dbg_msg("readbuf[%d] = %s", strlen(linebuf), linebuf);
        
        /*
         * The format of a line is:
         * <ip>:<port> linkspeed_1 linkspeed_2 ... linkspeed_prc_count
         * 
         * Line number determines the proc_id
         */

        tok = strtok(linebuf, TOK_DELIM);

        /* Skip empty lines or comments */
        if ( tok == NULL || tok[0] == '\0' || tok[0] == '#' ) {
            continue;
        }

        jx = 0;
        cnode = (*out_nodes) + ix;
        cnode->proc_id = ix;
        dbg_msg("cnode: offset from start of array is %4d bytes", (void*)cnode - (void*)(*out_nodes));

        /* Parse the IP */
        cnode->listen_addr.sin_family = AF_INET;
        inet_pton(AF_INET, tok, &cnode->listen_addr.sin_addr.s_addr);
        
        /* Parse the port */
        tok = strtok(NULL, TOK_DELIM);
        cnode->listen_addr.sin_port = htons(strtoul(tok, NULL, BASE_10));
        
        /* 
         * Only for this proc_id parse link speeds,
         * which can have suffixes of K,M,G case insensitive
         */
        if (ix == p_id && ix > 0) {
            while (jx < prc_count && NULL != (tok = strtok(NULL, TOK_DELIM))) {
                /* No error checking done here */
                lnk_speed = strtoull(tok, &mult, BASE_10) * speed_mult(*mult);
                dbg_msg("\t\t found link speed to node %2d: %10s = %llu", jx, tok, lnk_speed);
                
                cnode->link_speeds[jx++] = lnk_speed;
            }
            
            if (jx < prc_count) {
                dbg_err("There were only %d/%d links specified", jx, prc_count);
                fclose(fh);
                return ERR_BADFILE;
            }
            
            dbg_msg("Found %d/%d links for process %d\n", jx, prc_count, ix);
        }
        
        /* Set initial node state to IDLE */
        cnode->state = PS_IDLE;
        
        ix++;
    }
    fclose(fh);
    
    if (ix < prc_count) {
        /*
         * The file terminated unexpectedly.
         */
        dbg_err("File %s has only %d of %d records", fname, ix, prc_count);
        return ERR_BADFILE;
    }
    
    return 0;
}



int open_listen_socket (proc_id_t p_id, link_info_t * const nodes, size_t nodes_count)
{
    int res = 0;
    int max_nodes = nodes_count;
    
    if (p_id < 0 || p_id > max_nodes) {
        dbg_err("process id out of bounds: %llu not in [0..%d]", p_id, max_nodes);
        res = -1;
        goto end;
    }
    
    if (1 > (nodes[p_id].sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))) {
        dbg_err("Could not allocate socket");
        res = -1;
        goto end;
    }
    
    dbg_msg("The socket is open on fd %d", nodes[p_id].sock_fd);
    
    if (res = bind(nodes[p_id].sock_fd,
                   (const struct sockaddr *)&nodes[p_id].listen_addr,
                   sizeof(nodes[p_id].listen_addr))) {
        dbg_err("Could not bind socket.");
        goto end;
    }
    
end:
    /* There was an error so close the socket if created */
    if (res && nodes[p_id].sock_fd > 0) {
        close(nodes[p_id].sock_fd);
    }
    
    return res;
}

#define NANOS_PER_SECOND (1000 * 1000 * 1000)

uint64 get_msg_delay_nsec(uint64 link_speed, size_t msg_length)
{
    return (msg_length * NANOS_PER_SECOND / link_speed);
}

/*
 * Get delta between two timespec_t vars.
 */
timespec_t timespec_delta (timespec_t start, timespec_t end) {
	timespec_t temp;
	if ((end.tv_nsec - start.tv_nsec) < 0) {
		temp.tv_sec  = end.tv_sec  - start.tv_sec - 1;
		temp.tv_nsec = end.tv_nsec - start.tv_nsec + NANOS_PER_SECOND;
	} else {
		temp.tv_sec  = end.tv_sec  - start.tv_sec;
		temp.tv_nsec = end.tv_nsec - start.tv_nsec;
	}
	return temp;
}

/*
 * Computes average of some tspec deltas.
 * Be careful this works only for deltas because it overflows.
 * Don't feed it current time!
 */

timespec_t timespec_avg (timespec_t * tsarray, size_t len) {
    timespec_t avgts;
    int64 summ_sec = 0;
    int64 summ_nsec = 0;
    int64 avg_sec = 0;
    int64 avg_nsec = 0;
    size_t ix;

    avgts.tv_sec = 0;
    avgts.tv_nsec = 0;

    if (!tsarray || len == 0) {
        return avgts;
    }

    for (ix = 0 ; ix < len; ix++) {
        /* This will overflow if the tv_sec are too large */
        summ_sec += tsarray[ix].tv_sec;
        summ_nsec += tsarray[ix].tv_nsec;
    }

    avg_nsec = (summ_nsec + (summ_sec % len) * NANOS_PER_SECOND) / len;
    avg_sec = summ_sec / len;

    if (avg_nsec > NANOS_PER_SECOND) {
        avg_sec += avg_nsec / NANOS_PER_SECOND;
        avg_nsec = avg_nsec % NANOS_PER_SECOND;
    }

    avgts.tv_sec = avg_sec;
    avgts.tv_nsec = avg_nsec;

    return avgts;
}


