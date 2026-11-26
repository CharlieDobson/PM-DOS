/*
 * DRVSOCK.H - the SMBFS driver's socket shim (see DRVSOCK.C)
 *
 * The TCP conversation the SMB2 engine needs, over NDS_SOCKCALL instead
 * of INT 21h.  Timeouts are in milliseconds.
 */
#ifndef DRVSOCK_H
#define DRVSOCK_H

#include "ndrv.h"

int      dsock_up( uint32_t ms );                         /* 1 once the stack has an address */
void     dsock_nbname( char *out );                       /* TCPIP.SYS /NAME= into out[16], if one */
uint32_t dsock_resolve( const char *name, uint32_t ms );  /* an address, or 0 */
int      dsock_open( unsigned int type );                 /* a handle (>=1), or 0 */
void     dsock_close( int handle );
int      dsock_connect( int handle, uint32_t ip, uint16_t port, uint32_t ms ); /* 1 established */
int      dsock_send( int handle, const void *data, unsigned int len, uint32_t ms );  /* bytes sent */
int      dsock_recvwait( int handle, void *buf, unsigned int max, uint32_t ms );     /* bytes; 0 closed/timeout */

/* NetBIOS over NetBEUI, through NETBEUI.SYS (INC\NETBIOS.H) */
int      dnb_here( char *name );                               /* 1 when NETBEUI.SYS has its name, into name[16] */
uint32_t dnb_call( const char *name, uint8_t suffix, uint32_t ms ); /* a session handle, or 0 */
void     dnb_hangup( uint32_t handle );
int      dnb_send( uint32_t handle, const void *data, unsigned int len, uint32_t ms ); /* 1: the message is going */
unsigned dnb_wait( uint32_t handle, uint32_t ms );             /* the next message's length; 0 none */
unsigned dnb_recv( uint32_t handle, void *out, unsigned int max ); /* the next part of it */

/* Called from each wait loop when it has no progress to make: on the WfW
   bridge's worker fibre it yields the slice back to the poll (so Windows
   runs and the tick pumps the parked stack); run natively it does nothing
   and the loop spins as before.  Defined in SMBFS.C. */
void dsock_idle( void );

#endif /* DRVSOCK_H */
