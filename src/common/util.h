/*
 * src/common/util.h
 *
 * Common system routines used by all processes.
 * 
 *  Created on: Nov 23, 2010
 *      Author: alex
 * 
 */

#ifndef UTIL_H_
#define UTIL_H_

#include <string.h>
#include <time.h>

#include <common/defs.h>

#define BASE_10         10
#define BASE_16         16

typedef struct timespec timespec_t;
/*
 * Export functions in "util.c" to be available for other modules.
 */

extern int parse_peer_params(int argc, char * argv[],
                             uint64 *out_proc_id,
                             char ** out_fname);

extern int parse_sup_params(int argc, char * argv[],
                            char ** out_fname,
                            char ** out_logfname,
                            uint32 *out_concurency_ratio,
                            uint32 *out_concurent_count,
                            uint32 *out_election_interval);

extern int parse_file(const char * fname, proc_id_t p_id,
               link_info_t * out_nodes[], size_t * out_nodes_count);

extern int open_listen_socket (proc_id_t p_id, link_info_t * const nodes,
                               size_t nodes_count);

extern uint64 get_msg_delay_usec(uint64 link_speed, size_t msg_length);

extern timespec_t timespec_delta(timespec_t start, timespec_t end);
extern timespec_t timespec_avg(timespec_t * tsarray, size_t len);

#endif /* UTIL_H_ */
