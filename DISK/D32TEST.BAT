@ECHO OFF
REM D32TEST.BAT - run by COMMAND /C: 32-bit programs, started by the
REM 16-bit COMMAND.COM (the native bridge, VDM.INC vdm_d32).  Their
REM standard handles are COMMAND.COM's redirection, an errorlevel comes
REM back through them, and a DOS program started by one of them runs
REM on top of the box that started it.  SORT and CMD.EXE are D32;
REM ARGS.COM is the 16-bit one that exits with its first digit.  CRLF.
ECHO --- SORT reads what COMMAND.COM redirected into it
ECHO three> D32IN.TXT
ECHO one>> D32IN.TXT
ECHO two>> D32IN.TXT
SORT < D32IN.TXT
ECHO --- and writes where it redirected it to
SORT < D32IN.TXT > D32OUT.TXT
TYPE D32OUT.TXT
ECHO --- through a pipe
TYPE D32IN.TXT | SORT
ECHO --- CMD.EXE inside COMMAND.COM
CMD /C ECHO this is CMD.EXE, started by COMMAND.COM
ECHO --- an errorlevel from a DOS program inside CMD.EXE inside COMMAND.COM
CMD /C ARGS.COM 4
IF ERRORLEVEL 5 ECHO WRONG - errorlevel 5
IF ERRORLEVEL 4 ECHO errorlevel 4 came back through both shells
ECHO --- three shells deep
CMD /C COMMAND /C ECHO COMMAND.COM inside CMD.EXE inside COMMAND.COM
DEL D32IN.TXT
DEL D32OUT.TXT
ECHO --- D32TEST done
