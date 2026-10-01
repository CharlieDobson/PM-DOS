@ECHO OFF
REM XCTEST.CMD - XCOPY.EXE: every switch, the three questions, the
REM errors and the errorlevels, on a tree with long names in it.
REM
REM Run it with CMD /C and its output redirected to a log - it is a .CMD
REM so that quotes group a long name with spaces in it, redirections
REM included.  Everything happens in \XT on the current drive, which
REM must not exist beforehand.  The questions are answered from files
REM redirected into XCOPY, and XCEL.BAT says what errorlevel each left.
REM (No angle brackets in these REM lines: a REM line's redirection is
REM carried out, as DOS carries it out.)
REM
REM The fixture, \XT\SRC:
REM   A Long File Name.txt        plain
REM   SHORT.TXT                   plain
REM   RO.TXT                      read-only
REM   HID.TXT                     hidden
REM   Long Directory Name\mid.txt
REM   Long Directory Name\deeper\Deep File.dat
REM   empty dir\                  nothing in it
ECHO === XCTEST: making \XT ===
MD \XT
MD \XT\SRC
MD "\XT\SRC\Long Directory Name"
MD "\XT\SRC\Long Directory Name\deeper"
MD "\XT\SRC\empty dir"
ECHO hello long> "\XT\SRC\A Long File Name.txt"
ECHO short> \XT\SRC\SHORT.TXT
ECHO readonly> \XT\SRC\RO.TXT
ECHO hidden> \XT\SRC\HID.TXT
ECHO mid> "\XT\SRC\Long Directory Name\mid.txt"
ECHO deep> "\XT\SRC\Long Directory Name\deeper\Deep File.dat"
ATTRIB +R \XT\SRC\RO.TXT
ATTRIB +H \XT\SRC\HID.TXT
ECHO Y> \XT\Y.ANS
ECHO N> \XT\N.ANS
ECHO A> \XT\A.ANS
ECHO F> \XT\F.ANS
ECHO D> \XT\D.ANS
ECHO Y> \XT\YNY.ANS
ECHO N>> \XT\YNY.ANS
ECHO Y>> \XT\YNY.ANS

ECHO --- 1. a directory to a new one, /I: the top files, no hidden one
XCOPY \XT\SRC \XT\D1 /I
CALL \TESTS\XCEL.BAT
DIR \XT\D1 /B /ON
ECHO --- the read-only file's copy is not read-only, and has the archive bit
ATTRIB \XT\D1\RO.TXT

ECHO --- 2. /S into "D2\": the tree, but not the empty directory
XCOPY \XT\SRC \XT\D2\ /S
CALL \TESTS\XCEL.BAT
DIR \XT\D2 /B /S /ON
TYPE "\XT\D2\Long Directory Name\deeper\Deep File.dat"

ECHO --- 3. /E: the empty directory too
XCOPY \XT\SRC \XT\D3 /E /I /Q
CALL \TESTS\XCEL.BAT
DIR \XT\D3 /B /S /ON

ECHO --- 4. /H /K: the hidden file too, and the read-only one stays read-only
XCOPY \XT\SRC\*.TXT \XT\D4\ /H /K
CALL \TESTS\XCEL.BAT
ATTRIB \XT\D4\*.*

ECHO --- 5. /L lists and copies nothing
XCOPY \XT\SRC \XT\D5 /S /L /I
CALL \TESTS\XCEL.BAT
DIR \XT\D5 /B

ECHO --- 6. /T /E: every directory, no files; /T alone: those with a file
XCOPY \XT\SRC \XT\D6 /T /E /I
CALL \TESTS\XCEL.BAT
DIR \XT\D6 /B /S /ON
XCOPY \XT\SRC \XT\D6B /T /I
CALL \TESTS\XCEL.BAT
DIR \XT\D6B /B /S /ON

ECHO --- 7. /N: the copies get the short names
XCOPY \XT\SRC \XT\D7 /S /N /I
CALL \TESTS\XCEL.BAT
DIR \XT\D7 /B /S /ON

ECHO --- 8. /F: full names both sides
XCOPY \XT\SRC\SHORT.TXT \XT\D8\ /F
CALL \TESTS\XCEL.BAT

ECHO --- 9. a template: *.TXT to *.BAK in a new directory
XCOPY \XT\SRC\*.TXT \XT\D9\*.BAK
CALL \TESTS\XCEL.BAT
DIR \XT\D9 /B /ON

ECHO --- 10. overwriting: N, then Y, then /Y, then COPYCMD=/Y and /-Y
XCOPY \XT\SRC\SHORT.TXT \XT\D1 < \XT\N.ANS
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC\SHORT.TXT \XT\D1 < \XT\Y.ANS
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC\SHORT.TXT \XT\D1 /Y
CALL \TESTS\XCEL.BAT
SET COPYCMD=/Y
XCOPY \XT\SRC\SHORT.TXT \XT\D1
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC\SHORT.TXT \XT\D1 /-Y < \XT\N.ANS
CALL \TESTS\XCEL.BAT
SET COPYCMD=
ECHO --- ...and All: one answer for the rest
XCOPY \XT\SRC\*.TXT \XT\D1 < \XT\A.ANS
CALL \TESTS\XCEL.BAT

ECHO --- 11. /P: yes, no, yes
XCOPY \XT\SRC\*.TXT \XT\D11\ /P < \XT\YNY.ANS
CALL \TESTS\XCEL.BAT
DIR \XT\D11 /B /ON

ECHO --- 12. file or directory?  F, then D
XCOPY \XT\SRC\SHORT.TXT \XT\NEWNAME.TXT < \XT\F.ANS
CALL \TESTS\XCEL.BAT
TYPE \XT\NEWNAME.TXT
XCOPY \XT\SRC\SHORT.TXT \XT\NEWDIR < \XT\D.ANS
CALL \TESTS\XCEL.BAT
DIR \XT\NEWDIR /B

ECHO --- 13. /U: only what D9 already has - nothing, the names differ
XCOPY \XT\SRC \XT\D9 /U /Y
CALL \TESTS\XCEL.BAT
ECHO --- and what D1 has
XCOPY \XT\SRC \XT\D1 /U /Y
CALL \TESTS\XCEL.BAT

ECHO --- 14. /D: nothing is newer than its copy
XCOPY \XT\SRC \XT\D1 /D /Y
CALL \TESTS\XCEL.BAT

ECHO --- 15. /D:date - one far ahead, one long ago, one that is not a date
XCOPY \XT\SRC \XT\D15 /I /D:12-31-2099
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC \XT\D15 /I /D:1-1-90
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC \XT\D15 /I /D:13-45-2000
CALL \TESTS\XCEL.BAT

ECHO --- 16. /A and /M: archive bits
ATTRIB -A \XT\SRC\SHORT.TXT
XCOPY \XT\SRC\*.TXT \XT\D16\ /A
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC\*.TXT \XT\D16M\ /M
CALL \TESTS\XCEL.BAT
ATTRIB \XT\SRC\*.TXT
XCOPY \XT\SRC\*.TXT \XT\D16M\ /M
CALL \TESTS\XCEL.BAT

ECHO --- 17. errors: nothing found, a bad switch, too many and no names, no drive, no path
XCOPY \XT\NOSUCH.* \XT\D17\
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC \XT\D17 /Z
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC \XT\D17 \XT\D18
CALL \TESTS\XCEL.BAT
XCOPY
CALL \TESTS\XCEL.BAT
XCOPY Q:\X.TXT \XT
CALL \TESTS\XCEL.BAT
XCOPY \XT\NOPATH\X.TXT \XT
CALL \TESTS\XCEL.BAT

ECHO --- 18. a cyclic copy, and 19. a file onto itself
XCOPY \XT\SRC \XT\SRC\SUB /S /I
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC\SHORT.TXT \XT\SRC
CALL \TESTS\XCEL.BAT

ECHO --- 20. a read-only copy is not replaced without /R, 21. /C goes on
XCOPY \XT\SRC\RO.TXT \XT\D4\ /Y
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC\*.TXT \XT\D4\ /Y /C
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC\RO.TXT \XT\D4\ /Y /R
CALL \TESTS\XCEL.BAT

ECHO --- 22. reserved devices
XCOPY \XT\SRC\SHORT.TXT CON
CALL \TESTS\XCEL.BAT
XCOPY CON \XT\D22
CALL \TESTS\XCEL.BAT

ECHO --- 23. /W waits for a key, 24. /V is taken
XCOPY \XT\SRC\SHORT.TXT \XT\D23\ /W < \XT\Y.ANS
CALL \TESTS\XCEL.BAT
XCOPY \XT\SRC\SHORT.TXT \XT\D24\ /V
CALL \TESTS\XCEL.BAT

ECHO --- 25. a relative source: the names are shown relative too
CD \XT\SRC
XCOPY *.* \XT\D25\ /S
CALL \TESTS\XCEL.BAT
CD \

ECHO --- 26. a quoted long directory as the source, switches run together
XCOPY "\XT\SRC\Long Directory Name" \XT\D26/S/I
CALL \TESTS\XCEL.BAT
DIR \XT\D26 /B /S /ON

ECHO --- 27. no destination: the current directory
MD \XT\D27
CD \XT\D27
XCOPY \XT\SRC\SHORT.TXT
CALL \TESTS\XCEL.BAT
DIR /B
CD \

ECHO --- 28. /?
XCOPY /?
CALL \TESTS\XCEL.BAT

ECHO --- 29. ^C at the /W wait: errorlevel 2, and nothing copied
HOOKTEST /C
XCOPY \XT\SRC\SHORT.TXT \XT\D29\ /W
CALL \TESTS\XCEL.BAT
DIR \XT\D29 /B

ECHO --- 30. ^C two levels down at a program with no handler of its own:
ECHO ---     LABEL is ended, and the label is what it was
HOOKTEST /C
LABEL C:
VOL C:

ECHO --- 31. a file of many buffers - PMDOS.SYS, hidden and system, so /H /K -
ECHO ---     and an empty one.  The harness compares the big one byte for byte.
MD \XT\BIG
TYPE NUL > \XT\BIG\ZERO.TXT
XCOPY \PMDOS.SYS \XT\BIG\ /H /K
CALL \TESTS\XCEL.BAT
XCOPY \XT\BIG\ZERO.TXT \XT\D31\
CALL \TESTS\XCEL.BAT
DIR \XT\BIG /A /ON
DIR \XT\D31 /ON
ECHO === XCTEST done ===
