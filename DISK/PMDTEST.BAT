@ECHO OFF
REM PMDTEST.BAT - PMD.EXE, driven by key files, in \PMDT on the current
REM drive (which must not exist beforehand).  Run it with CMD /C and its
REM output redirected to a log.
REM
REM PMD reads its keys from standard input when PMDTEST names a file and
REM input is redirected, and writes its screen into that file wherever
REM the key file asks - TOOLS\MKPMDKEYS.PL writes the key files, and its
REM comments say what each session does.  What a page says depends on
REM the machine, so this is a log to read and not one to compare with
REM another machine's.
MD \PMDT
CD \PMDT
ECHO BUFFERS=10> INS.TXT
ECHO FILES=30>> INS.TXT

ECHO === PMD /?
PMD /?
ECHO === PMD /S: the summary
PMD /S
ECHO === PMD /S file, /P file
PMD /S C:\PMDT\SUM.TXT
TYPE SUM.TXT
PMD /P C:\PMDT\FULL.TXT
TYPE FULL.TXT
ECHO === a switch PMD has not got, and a report with no file
PMD /Z
PMD /P

ECHO === PMD1: the main screen and every page
SET PMDTEST=C:\PMDT\PMD1.SCR
PMD < C:\TESTS\PMD1.KEY
TYPE PMD1.SCR

ECHO === PMD2: the menus, a file, the mouse
SET PMDTEST=C:\PMDT\PMD2.SCR
PMD < C:\TESTS\PMD2.KEY
TYPE PMD2.SCR

ECHO === PMD3: Memory Block Display, Memory Browser, Insert Command, Test Printer
SET PMDTEST=C:\PMDT\PMD3.SCR
PMD < C:\TESTS\PMD3.KEY
TYPE PMD3.SCR

ECHO === PMD4: Find File and Print Report
SET PMDTEST=C:\PMDT\PMD4.SCR
PMD < C:\TESTS\PMD4.KEY
TYPE PMD4.SCR
ECHO --- RPT.TXT:
TYPE RPT.TXT
ECHO --- (end of RPT.TXT)

ECHO === PMD5: Insert Command
SET PMDTEST=C:\PMDT\PMD5.SCR
PMD < C:\TESTS\PMD5.KEY
TYPE PMD5.SCR
ECHO --- INS.TXT now:
TYPE INS.TXT
ECHO --- (end of INS.TXT)

ECHO === PMD /I: nothing looked at until it is asked for
SET PMDTEST=C:\PMDT\PMD6.SCR
PMD /I < C:\TESTS\PMD1.KEY
TYPE PMD6.SCR
SET PMDTEST=
CD \
ECHO === PMDTEST done
