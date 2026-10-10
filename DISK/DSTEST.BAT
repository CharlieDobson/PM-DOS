@ECHO OFF
REM DSTEST.BAT - DOSSHELL.EXE, driven by key files, in \DS on the current
REM drive (which must not exist beforehand).  Run it with CMD /C and its
REM output redirected to a log.
REM
REM DOSSHELL reads its keys from standard input when DSTEST names a file
REM and input is redirected, and writes its screen into that file wherever
REM the key file asks - TOOLS\MKDSKEYS.PL writes the key files, and its
REM comments say what each session does and must leave behind.  A test
REM run reads and writes no DOSSHELL.INI unless DSINI names one, so the
REM settings of whoever runs this are not touched.
REM
REM The tree lists every directory of the drive, so what it shows is this
REM disk's: a log to read, not one to compare with another machine's.
MD \DS
MD \DS\ONE
MD \DS\ONE\DEEP
MD \DS\TWO
ECHO alpha> \DS\ONE\A.TXT
ECHO beta beta> \DS\ONE\B.TXT
ECHO gamma> \DS\ONE\C.DAT
ECHO deep> \DS\ONE\DEEP\D.TXT
CD \DS\ONE

ECHO === DOSSHELL /?
DOSSHELL /?
ECHO === DOSSHELL /Q (no such switch)
DOSSHELL /Q

ECHO === DS1: the screen, the menus, help, About
SET DSTEST=\DS\DS1.SCR
DOSSHELL /T < C:\TESTS\DS1.KEY
TYPE \DS\DS1.SCR

ECHO === DS2: copy, move over a file, rename, attributes, delete, a new directory
SET DSTEST=\DS\DS2.SCR
DOSSHELL /T < C:\TESTS\DS2.KEY
TYPE \DS\DS2.SCR
ECHO --- \DS\ONE now:
DIR \DS\ONE
ATTRIB \DS\ONE\*.*
ECHO --- \DS\TWO now:
DIR \DS\TWO
ATTRIB -R \DS\ONE\FIRST.TXT
REN \DS\ONE\FIRST.TXT A.TXT

ECHO === DS3: the tree, the views, Search, Show Information, the options
SET DSTEST=\DS\DS3.SCR
DOSSHELL /T < C:\TESTS\DS3.KEY
TYPE \DS\DS3.SCR

ECHO === DS4: the program list - a group, a new entry and its question, Reorder, Delete
SET DSTEST=\DS\DS4.SCR
DOSSHELL /T < C:\TESTS\DS4.KEY
TYPE \DS\DS4.SCR

ECHO === DS5: the file viewer, and Run
SET DSTEST=\DS\DS5.SCR
DOSSHELL /T < C:\TESTS\DS5.KEY
TYPE \DS\DS5.SCR

ECHO === DS6: the mouse
SET DSTEST=\DS\DS6.SCR
DOSSHELL /T < C:\TESTS\DS6.KEY
TYPE \DS\DS6.SCR

ECHO === DS7: /G:M1, a graphics screen
SET DSTEST=\DS\DS7.SCR
DOSSHELL /G:M1 < C:\TESTS\DS7.KEY
TYPE \DS\DS7.SCR
ECHO --- the picture DS7 wrote, when the screen was a graphics one:
DIR \DS\DS701.BMP

ECHO === DS8: settings saved in the file DSINI names
SET DSINI=\DS\TEST.INI
SET DSTEST=\DS\DS8.SCR
DOSSHELL /T < C:\TESTS\DS8.KEY
TYPE \DS\DS8.SCR
ECHO --- \DS\TEST.INI:
TYPE \DS\TEST.INI

ECHO === DS9: ...and read back
SET DSTEST=\DS\DS9.SCR
DOSSHELL < C:\TESTS\DS9.KEY
TYPE \DS\DS9.SCR
SET DSINI=
SET DSTEST=
CD \
ECHO === DSTEST done
