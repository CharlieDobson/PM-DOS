@ECHO OFF
ECHO %0 args [%1] [%2]
SET N=first
ECHO env N is %N%
ECHO literal 100%% done
IF "%1"=="" ECHO   empty-arg branch taken
IF NOT "%1"=="" ECHO   non-empty branch taken
GOTO SKIP
ECHO   THIS LINE MUST NOT PRINT
:SKIP
ECHO after forward GOTO
IF "%1"=="loop" GOTO DONE
ECHO calling myself recursively
CALL GOTOTEST loop
ECHO returned from the recursive CALL
:DONE
ECHO GOTOTEST end
