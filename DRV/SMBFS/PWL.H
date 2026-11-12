/*
 * PWL.H - the password list (NET LOGON), DRV\SMBFS\PWL.C.  Needs NDRV.H
 * and REDIR.H before it.
 */
#ifndef PWL_H
#define PWL_H

/* a REDIR_PWL request: the answer is in q->status */
void pwl_call( pwl_req *q );

/* the saved password for a connection: by the domain, or by the server
   when the connection names no domain; and by the user, when one is
   given.  Fills an empty user with the saved one.  1 found, 0 not (or no
   list unlocked). */
int  pwl_fill( const char *server, const char *domain, char *user, int usermax,
               char *pass, int passmax );

#endif /* PWL_H */
