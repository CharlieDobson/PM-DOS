/*
 * PROCESS.H - running other programs
 *
 * DOS runs one program at a time, so a spawned program runs to its end
 * before the caller goes on: P_WAIT returns its exit code, P_OVERLAY
 * runs it and then ends the caller with that code, and P_NOWAIT is
 * refused.  The command line a program is given holds 126 characters
 * at most.
 *
 * The p forms look along PATH, as the shell does, for a name with no
 * drive or directory in it, and try .COM, .EXE and .BAT after it when
 * it has no extension.  A .BAT file is run by the shell COMSPEC names.
 */
#ifndef _PROCESS_H
#define _PROCESS_H

#include <_pmdefs.h>

#define P_WAIT          0
#define P_NOWAIT        1
#define P_OVERLAY       2

_PM_BEGIN_C

int     spawnl( int mode, const char *path, const char *arg0, ... );
int     spawnle( int mode, const char *path, const char *arg0, ... );
int     spawnlp( int mode, const char *path, const char *arg0, ... );
int     spawnlpe( int mode, const char *path, const char *arg0, ... );
int     spawnv( int mode, const char *path, char *const argv[] );
int     spawnve( int mode, const char *path, char *const argv[], char *const envp[] );
int     spawnvp( int mode, const char *path, char *const argv[] );
int     spawnvpe( int mode, const char *path, char *const argv[], char *const envp[] );

int     execl( const char *path, const char *arg0, ... );
int     execv( const char *path, char *const argv[] );
int     execvp( const char *path, char *const argv[] );
int     execve( const char *path, char *const argv[], char *const envp[] );

int     getpid( void );

_PM_END_C

#endif
