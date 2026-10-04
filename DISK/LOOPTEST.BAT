@ECHO OFF
ECHO -- walking the argument list with SHIFT --
:ARGLOOP
IF "%1"=="" GOTO ARGSDONE
ECHO   arg: %1
SHIFT
GOTO ARGLOOP
:ARGSDONE
ECHO -- all arguments consumed --
ECHO -- CTTY --
CTTY CON
ECHO CTTY CON accepted
REM
REM "CTTY AUX" USED TO BE THE NEXT LINE AND IT ENDED THE SESSION.
REM On a machine with nothing on the serial port it leaves the
REM interpreter reading a device that has nothing to say, with no
REM keyboard left to type CTTY CON on - which is authentic, is
REM documented in cmd_ctty, and is the oldest joke in DOS.  What it
REM is NOT is a test: the "-- done --" below went to AUX, so the one
REM thing the line was there to show could never be seen, and any
REM unattended run that reached it hung for ever with this screen as
REM its last word.  Two of them did.  The CTTY CON above is the
REM check that can actually pass.
ECHO -- done --
