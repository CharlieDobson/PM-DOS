@ECHO OFF
ECHO --- a 16-bit .COM, started by COMMAND.COM ---
HELLO
ECHO --- a 16-bit .EXE, relocated by the loader ---
HELLOX
ECHO --- the command tail, the FCBs and the environment ---
ARGS one two
ECHO --- a bad drive in the first argument, a wildcard in the second ---
ECHO     (expect AX=FF01)
ARGS Q:THING *.TXT
ECHO --- the exit code comes back through EXEC ---
ARGS 3
IF ERRORLEVEL 3 IF NOT ERRORLEVEL 4 ECHO OK: ERRORLEVEL 3
IF NOT ERRORLEVEL 3 ECHO FAIL: the exit code did not come back
ECHO --- a 16-bit program redirected into a file ---
HELLO > VDM1.TXT
TYPE VDM1.TXT
DEL VDM1.TXT
ECHO --- and piped into an external ---
ARGS four five | SORT
ECHO --- the A20 gate, reached through the hardware ---
A20TEST
IF ERRORLEVEL 1 ECHO FAIL: A20TEST reported a problem
ECHO --- a 16-bit program starting a 16-bit program ---
EXECTST
IF ERRORLEVEL 1 ECHO FAIL: EXECTST reported a problem
ECHO --- terminate and stay resident, across two DOS boxes ---
TSRTEST
IF ERRORLEVEL 1 ECHO FAIL: TSRTEST could not install
TSRTEST
IF ERRORLEVEL 1 ECHO FAIL: the resident copy was not found
ECHO --- one cursor between the BIOS and the console, not two ---
CURTEST
ECHO --- the disk BIOS, and whether it agrees with the filesystem ---
I13TEST
ECHO --- a 16-bit program that asks to be put into protected mode ---
DPMITEST
ECHO VDMTEST done
