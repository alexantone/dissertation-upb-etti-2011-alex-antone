/*
 * src/common/init.h
 *
 * Signal and interrupt handling. This is the home of the signaling queue system.
 * 
 *  Created on: Nov 22, 2010
 *      Author: alex
 * -------------------------------------------------------------------------
 */
#ifndef INIT_H_
#define INIT_H_

#include <netinet/in.h>
#include <sys/socket.h>

#include <common/defs.h>

typedef int (ev_handler_fnct_t)(void * cookie);

extern const char * evtostr(dme_ev_t event);
extern const char * sigrttostr(unsigned int signo);

extern int  init_handlers(int sock);
extern int  deinit_handlers(void);

extern void register_event_handler_(dme_ev_t event, ev_handler_fnct_t func,
                                    char * funcname);
#define register_event_handler(ev, fn) register_event_handler_(ev, fn, #fn)

extern int  queue_event(dme_ev_t event, void * cookie);
extern int  handle_event(dme_ev_t event, void * cookie);
extern int  schedule_event (dme_ev_t event, uint32 secs, uint32 nsecs,void * cookie);

void wait_events(void);


#endif /* INIT_H_ */
