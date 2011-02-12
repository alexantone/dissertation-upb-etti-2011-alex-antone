/*
 * src/common/fsm.c
 *
 * Critical region checks used by all programs.
 * 
 *  Created on: Dec 13, 2010
 *      Author: alex
 * -------------------------------------------------------------------------
 */

#include <common/fsm.h>

extern proc_id_t proc_id;                  /* this process id */
extern link_info_t * nodes;                /* peer matrix */
extern size_t nodes_count;

extern int err_code;
extern bool_t exit_request;

/*
 * Checks if all processes are in IDLE state, thus not having any interest
 * in the critical region for now.
 */
bool_t critical_region_is_idle(void) {
    bool_t is_idle = TRUE;
    int ix = 1;         /* we skip the supervisor */
    
    while (is_idle && ix <= nodes_count) {
        is_idle = is_idle && (nodes[ix++].state == PS_IDLE);
    }
    return is_idle;
}

/*
 * Checks if the critical region is free.
 */
bool_t critical_region_is_free(void) {
    bool_t is_free = TRUE;
    int ix = 1;         /* we skip the supervisor */
    
    while (is_free && ix <= nodes_count) {
        is_free = is_free && (nodes[ix++].state != PS_EXECUTING);
    }
    return is_free;
}


/*
 * Gets the number of processes that wait for the critical region to be freed
 */
int critical_region_pending_get_count(void) {
    int count = 0;
    int ix;
    
    for (ix = 1; ix <= nodes_count; ix++) {
        if (nodes[ix].state == PS_PENDING) {
            count++; 
        }
    }
    
    return count;
}

/*
 * Oh, well: event thought it should not happen chec if there are more than
 * 1 proceses in the critical region (fatality 8X).
 */
        
bool_t critical_region_is_sane(void) {
    int ix = 1;          /* we skipt he supervisor */
    int count = 0;       /* count how many there are inside the critical region */
    
    while (count < 2 && ix++ <= nodes_count) {
        if (nodes[ix].state == PS_EXECUTING) {
            count++; 
        }
    }
    
    return (count < 2);
}
