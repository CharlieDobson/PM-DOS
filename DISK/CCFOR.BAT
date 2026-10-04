@ECHO OFF
REM CCFOR.BAT - typed at an interactive COMMAND.COM (HOOKTEST /T types
REM it).  A child ended by ^C stops the FOR that ran it, whatever the
REM answer to "Terminate batch job" - N here, typed by HOOKTEST /I once
REM the question is asked - and the batch file carries on.  But a FOR
REM that CALLs a file whose child is ended runs on: that FOR is the
REM calling level's, and the ^C is the called one's.  Both checked
REM against MS-DOS 6.22's COMMAND.COM (ContCTerm clears ForFlag for the
REM level it is in).  CRLF, for 6.22, which reads a line up to CR.
ECHO --- a FOR whose child is ended by ^C runs once >> C:\COUT.TXT
FOR %%F IN (1 2 3) DO HOOKTEST /D >> C:\COUT.TXT
ECHO --- a FOR that CALLs a file whose child is ended runs on >> C:\COUT.TXT
FOR %%F IN (1 2 3) DO CALL CCONE.BAT %%F
ECHO --- the batch file carried on >> C:\COUT.TXT
EXIT
