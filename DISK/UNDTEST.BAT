@ECHO OFF
REM UNDTEST.BAT - delete protection (UNDELETE=ON) and UNDELETE.EXE
REM
REM   CMD /C UNDTEST.BAT > C:\UOUT.TXT
REM
REM Each check prints OK or FAIL; the listings between them are there
REM to be read.  It starts by emptying C:\RECYCLED for good, so run it
REM on a test disk.  It wants FCBTEST.COM and LFNAPI.COM on the PATH
REM and COMMAND.COM in the root, and about 9MB free.
REM
REM The allowance check wants C: to have between 25MB and 36MB free
REM once nothing is kept - true of a 32MB test disk - so that the
REM allowance, a tenth of that, lies between B4's 2.4MB and B3+B4's
REM 3.6MB.  B5 (4.8MB) is then too big to keep, and keeping B4 has to
REM let go of both B2 and B3, which were deleted before it.  B0 and B1
REM are .TMP files and are never kept at all.
C:
CD \
UNDELETE C: /PURGE
MD \UNDT
CD \UNDT
ECHO --- status
UNDELETE /STATUS

ECHO --- a 32-bit DEL is kept; .TMP and .$$$ files are not
ECHO one> ONE.TXT
ECHO temp> JUNK.TMP
ECHO pipe> PIPE.$$$
DEL ONE.TXT
DEL JUNK.TMP
DEL PIPE.$$$
IF NOT EXIST ONE.TXT ECHO OK   ONE.TXT has left its directory
IF EXIST ONE.TXT ECHO FAIL ONE.TXT is still there
UNDELETE /LIST
ATTRIB \RECYCLED\*.*

ECHO --- /ALL puts it back
UNDELETE /ALL
IF EXIST ONE.TXT ECHO OK   ONE.TXT is back
IF NOT EXIST ONE.TXT ECHO FAIL ONE.TXT is not back
TYPE ONE.TXT
UNDELETE /LIST

ECHO --- a name taken since comes back with a # in it
DEL ONE.TXT
ECHO new> ONE.TXT
UNDELETE ONE.TXT /ALL
IF EXIST #NE.TXT ECHO OK   #NE.TXT
IF NOT EXIST #NE.TXT ECHO FAIL no #NE.TXT
TYPE #NE.TXT
TYPE ONE.TXT

ECHO --- a long name comes back long (a batch file takes a long name
ECHO --- as typed, quotes and all, so this one has no spaces)
ECHO long> LongNameOfAFile.txt
DIR LONGNA~1.TXT
DEL LongNameOfAFile.txt
IF NOT EXIST LongNameOfAFile.txt ECHO OK   deleted
UNDELETE LongName* /LIST
UNDELETE LongName* /ALL
IF EXIST LongNameOfAFile.txt ECHO OK   the long name is back
IF NOT EXIST LongNameOfAFile.txt ECHO FAIL the long name is not back
DIR LONGNA~1.TXT

ECHO --- a directory that has gone is made again
MD SUB
ECHO sub> SUB\S.TXT
DEL SUB\S.TXT
RD SUB
IF NOT EXIST SUB\S.TXT ECHO OK   SUB has gone
UNDELETE SUB\*.* /ALL
IF EXIST SUB\S.TXT ECHO OK   SUB\S.TXT is back
IF NOT EXIST SUB\S.TXT ECHO FAIL SUB\S.TXT is not back

ECHO --- /S from above, and a Y from the keyboard
ECHO Y> Y.TMP
DEL SUB\S.TXT
CD \
UNDELETE \UNDT /S /LIST
UNDELETE \UNDT\SUB < \UNDT\Y.TMP
CD \UNDT
IF EXIST SUB\S.TXT ECHO OK   answered Y, and it is back
IF NOT EXIST SUB\S.TXT ECHO FAIL answered Y, and it is not back
DEL Y.TMP

ECHO --- 16-bit programs: COMMAND.COM's DEL, and FCBTEST's FCB delete
ECHO two> TWO.TXT
COMMAND /C DEL TWO.TXT
IF NOT EXIST TWO.TXT ECHO OK   COMMAND.COM deleted TWO.TXT
FCBTEST
UNDELETE /LIST
UNDELETE TWO.TXT /ALL
IF EXIST TWO.TXT ECHO OK   TWO.TXT is back
IF NOT EXIST TWO.TXT ECHO FAIL TWO.TXT is not back

ECHO --- 7141h: LFNAPI deletes a long-named file, which is kept
LFNAPI
CD \UNDT
UNDELETE \ /S /LIST

ECHO --- the allowance
COPY /B C:\CMD.EXE+C:\CMD.EXE+C:\COMMAND.COM B0.TMP > NUL
COPY /B B0.TMP+B0.TMP B1.TMP > NUL
COPY /B B1.TMP+B1.TMP B2.BIN > NUL
COPY /B B2.BIN+B2.BIN B3.BIN > NUL
COPY /B B3.BIN+B3.BIN B4.BIN > NUL
COPY /B B4.BIN+B4.BIN B5.BIN > NUL
DIR B*.*
DEL B0.TMP
DEL B1.TMP
DEL B5.BIN
UNDELETE B5.BIN /LIST
DEL B2.BIN
DEL B3.BIN
UNDELETE B?.* /LIST
DEL B4.BIN
UNDELETE B?.* /LIST
UNDELETE /STATUS

ECHO --- everything kept on C:
UNDELETE \ /S /LIST

ECHO --- /PURGE
UNDELETE /PURGE
UNDELETE /STATUS
UNDELETE /LIST
IF NOT EXIST \RECYCLED\UNDELETE.DAT ECHO OK   the index has gone

ECHO --- what is deleted from inside RECYCLED is not kept again
ECHO three> THREE.TXT
DEL THREE.TXT
DEL \RECYCLED\*.TXT
UNDELETE \RECYCLED /LIST
UNDELETE /LIST
UNDELETE /PURGE
ECHO --- done
