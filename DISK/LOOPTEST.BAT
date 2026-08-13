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
CTTY AUX
ECHO -- done --
