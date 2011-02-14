/*
 * src/common/defs.h
 *
 * Base definitions file.
 * 
 *  Created on: Nov 6, 2010
 *      Author: alex
 * -------------------------------------------------------------------------
 */

#ifndef DEFS_H_
#define DEFS_H_

#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <netinet/in.h>

/* Warning messages*/
#define ERR_WARN            0x1000
#define ERR_BAD_MAGIC       0x1001
#define ERR_BAD_SUP_MAGIC   0x1002
#define ERR_BAD_DME_MAGIC   0x1003
#define ERR_BAD_PEER_ID     0x1004
#define ERR_SEND_MSG        0x1005
#define ERR_RECV_MSG        0x1006
#define ERR_DME_HDR         0x1007
#define ERR_SUP_HDR         0x1008

/* Errors above ERR_FATAL force quitting the program */
#define ERR_FATAL       0x2000
#define ERR_BADARGS     0x2001
#define ERR_BADFILE     0x2002
#define ERR_MALLOC      0x2003
#define ERR_INIT        0x2004

/* Data typedefs */
typedef int8_t  int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

typedef u_int8_t  uint8;
typedef u_int16_t uint16;
typedef u_int32_t uint32;
typedef u_int64_t uint64;

/* Process IDs definitions */
typedef uint64 proc_id_t;
#define MAX_DME_PROCS (100)
#define SUPERVISOR_PID (0)

#define PACKED __attribute__((__packed__))

/* symbolic names to speeds */
#define KBITPS (1 << 10)
#define MBITPS (1 << 20)
#define GBITPS (1 << 30)

typedef enum process_state_e {
    PS_IDLE,            /* Out of critical region, and not interested */
    PS_PENDING,         /* Waiting to enter the critical region */        
    PS_EXECUTING,       /* Inside critical region */
} process_state_t;

typedef enum dme_evt_e {
    DME_EV_PEER_MSG_IN,
    DME_EV_SUP_MSG_IN,
    DME_EV_WANT_CRITICAL_REG,
    DME_EV_ENTERED_CRITICAL_REG,
    DME_EV_EXITED_CRITICAL_REG,

    /* These events are used by the supervisor */
#define DME_SEV_MSG_IN DME_EV_SUP_MSG_IN   /* the supervisor uses only SUP messages */
    DME_SEV_PERIODIC_WORK,
    DME_SEV_SYNCRO,
    DME_SEV_ENDSIMULATION,
    
    /* 
     * Events greater than are DME_INTERNAL_EV_START registered statically.
     * If trying to register another handler an erro will occur.
     */
    DME_INTERNAL_EV_START,
    
    /* Internal events. Must not be used by the user */
    DME_IEV_PACK_IN,            /* Used by peer processes */
    
    /* This means the event id is invalid */
    DME_EV_INVALID,
} dme_ev_t;


/* Info to be included in adjacency matrix cells */
typedef struct link_info_s {
    uint64 link_speeds[MAX_DME_PROCS];  /* Link speeds in bps to other nodes */
    struct sockaddr_in listen_addr;     /* Address on which current process listens */
    int sock_fd;                        /* The socket bound to the listen address */
    proc_id_t proc_id;                  /* Process ID */
    process_state_t state;              /* Current process state */
} link_info_t;

typedef struct buff_s {
    uint8 * data;
    uint32 len;
} buff_t;

/*
 * Debugging macros
 */
#define DEBUGING_ENABLED 0
#if DEBUGING_ENABLED

#define dbg_msg(format, args...) \
fprintf(stdout, "debug: %24s:%4d %24s() -> " format "\n",\
		__FILE__, __LINE__, __func__, ##args)

#define dbg_err(format, args...) \
fprintf(stderr, "error: %24s:%4d %24s() -> " format "\n",\
		__FILE__, __LINE__, __func__, ##args)

#else

#define dbg_msg(format, args...) (NULL)
#define dbg_err(format, args...) fprintf(stderr, "#error: " format "\n", ##args)

#endif
/* 
 * Safe free.
 * Can be called on NULL pointers.
 * Render the pointer NULL after call
 */
#define safe_free(p) (p ? free(p) : \
                          dbg_msg("You tried to free pointer "#p"=NULL! But we forgive you..."),\
                      p = NULL )

/*
 * Gets a member's offset in bytes inside a structure.
 */
#define offsetof(st, m) \
    ((size_t) ( (char *)&((st *)(0))->m - (char *)0 ))

/*
 * Gets the number of elements in a statically initialized array
 */
#define get_count(array) (sizeof(array) / sizeof(array[0]))

#define TRUE    (1)
#define FALSE   (0)
typedef u_int8_t  bool_t;


#endif /* DEFS_H_ */
