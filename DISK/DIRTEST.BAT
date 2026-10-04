@ECHO OFF
REM DIRTEST.BAT - DIR's switches: /A /B /L /O /C /4 and DIRCMD.
REM
REM Every line ends ">> %LOG%" so that a harness can collect the lot
REM into one file - CALL does not carry redirection into a batch file,
REM so it has to be written on each line.  Run by hand, LOG is CON.
REM
REM The fixture, in \DT on the current drive:
REM   A.TXT   plain
REM   B.TXT   read-only
REM   C.DOC   hidden
REM   SUB\    a directory, with D.TXT in it
IF "%LOG%"=="" SET LOG=CON
ECHO === DIRTEST: making \DT === >> %LOG%
MD \DT
MD \DT\SUB
ECHO one> \DT\A.TXT
ECHO two two> \DT\B.TXT
ECHO three three three> \DT\C.DOC
ECHO four> \DT\SUB\D.TXT
ATTRIB +R \DT\B.TXT >> %LOG%
ATTRIB +H \DT\C.DOC >> %LOG%
ECHO --- DIR /B: names only, no . or .., no hidden file >> %LOG%
DIR \DT /B /ON >> %LOG%
ECHO --- DIR /B /S: whole paths, down the tree >> %LOG%
DIR \DT /B /S /ON >> %LOG%
ECHO --- DIR /A /B: the hidden file too >> %LOG%
DIR \DT /A /B /ON >> %LOG%
ECHO --- DIR /AH /B: only the hidden file >> %LOG%
DIR \DT /AH /B >> %LOG%
ECHO --- DIR /A-D /B: files, hidden included, no directory >> %LOG%
DIR \DT /A-D /B /ON >> %LOG%
ECHO --- DIR /A:R /B: only the read-only file >> %LOG%
DIR \DT /A:R /B >> %LOG%
ECHO --- DIR /AD: directories, in full >> %LOG%
DIR \DT /AD /ON >> %LOG%
ECHO --- DIR /L /B /ON: lower case >> %LOG%
DIR \DT /L /B /ON >> %LOG%
ECHO --- DIR /A /B /O-N: backwards >> %LOG%
DIR \DT /A /B /O-N >> %LOG%
ECHO --- DIR /OS /B: by size >> %LOG%
DIR \DT /OS /B /A-D >> %LOG%
ECHO --- DIR /C /CH /4 /OC: taken, and changing nothing >> %LOG%
DIR \DT /C /CH /4 /OC >> %LOG%
ECHO --- DIR /W /B: bare wins >> %LOG%
DIR \DT /W /B /ON >> %LOG%
ECHO --- DIR /AX: refused >> %LOG%
DIR \DT /AX >> %LOG%
ECHO --- DIR of nothing matching, bare >> %LOG%
DIR \DT\*.QQQ /B >> %LOG%
ECHO --- DIRCMD=/B /L, then /-B on the line >> %LOG%
SET DIRCMD=/B /L /ON
DIR \DT >> %LOG%
DIR \DT /-B >> %LOG%
SET DIRCMD=
ECHO --- VOL: the drive letter is in it now >> %LOG%
VOL >> %LOG%
ECHO --- tidying up >> %LOG%
ATTRIB -R \DT\B.TXT >> %LOG%
ATTRIB -H \DT\C.DOC >> %LOG%
DEL \DT\SUB\D.TXT
RD \DT\SUB
DEL \DT\A.TXT
DEL \DT\B.TXT
DEL \DT\C.DOC
RD \DT
ECHO === DIRTEST: done === >> %LOG%
