@ECHO OFF
REM BOXTEST.BAT - run by COMMAND /C.  Two things that end a program a
REM 16-bit COMMAND.COM started, and after each the shell has to be the
REM one that carries on: a DPMI client exiting, and a ^C with no handler
REM of its own.  The ^C makes COMMAND.COM ask "Terminate batch job" and
REM HOOKTEST /D has left an N in the keyboard buffer to answer it.
REM
REM CRLF, for MS-DOS 6.22's COMMAND.COM, which reads a line up to CR.
DPMITEST > NUL
ECHO COMMAND.COM is still here after a DPMI client ended
HOOKTEST /D
ECHO COMMAND.COM is still here after a ^C ended its child
