@ECHO OFF
REM RENTEST.BAT - REN with wildcards, REN and MOVE on a directory, and
REM COPY's /Y, /-Y and COPYCMD.  In \RT on the current drive.
REM
REM Every line ends ">> %LOG%", for DIRTEST.BAT's reason.
REM
REM COPY's questions are answered from files: Y.ANS holds a Y and N.ANS
REM an N, and "< N.ANS" is how a batch file says No to a prompt.  In a
REM batch file COPY does not ask at all unless /-Y or COPYCMD=/-Y says
REM to, which is the first thing checked.
IF "%LOG%"=="" SET LOG=CON
ECHO === RENTEST: making \RT === >> %LOG%
MD \RT
ECHO a> \RT\ONE.TXT
ECHO b> \RT\TWO.TXT
ECHO c> \RT\THREE.DOC
ECHO Y> \RT\Y.ANS
ECHO N> \RT\N.ANS
ECHO --- REN *.TXT *.BAK >> %LOG%
REN \RT\*.TXT *.BAK >> %LOG%
DIR \RT /B /ON >> %LOG%
ECHO --- REN ONE.BAK X?.*: a literal uses a place up, ? keeps one >> %LOG%
REN \RT\ONE.BAK X?.* >> %LOG%
DIR \RT\*.BAK /B /ON >> %LOG%
ECHO --- REN *.BAK Y*.*: collected first, so none renamed twice >> %LOG%
REN \RT\*.BAK Y*.* >> %LOG%
DIR \RT\*.BAK /B /ON >> %LOG%
ECHO --- REN with a path in the new name: refused >> %LOG%
REN \RT\THREE.DOC \THREE.DOC >> %LOG%
ECHO --- REN THREE.DOC FOUR.DOC stays in \RT >> %LOG%
REN \RT\THREE.DOC FOUR.DOC >> %LOG%
DIR \RT\*.DOC /B >> %LOG%
ECHO --- REN a name that is not there >> %LOG%
REN \RT\NOSUCH.TXT OTHER.TXT >> %LOG%
ECHO --- REN a directory >> %LOG%
MD \RT\OLDDIR
ECHO in the directory> \RT\OLDDIR\INSIDE.TXT
REN \RT\OLDDIR NEWDIR >> %LOG%
DIR \RT /AD /B >> %LOG%
TYPE \RT\NEWDIR\INSIDE.TXT >> %LOG%
ECHO --- MOVE renames a directory too >> %LOG%
MOVE \RT\NEWDIR \RT\MOVDIR >> %LOG%
DIR \RT /AD /B >> %LOG%
TYPE \RT\MOVDIR\INSIDE.TXT >> %LOG%
ECHO --- REN in place: B.TXT becomes Z.TXT where B.TXT was, so A Z C >> %LOG%
MD \RT\ORD
ECHO a> \RT\ORD\A.TXT
ECHO b> \RT\ORD\B.TXT
ECHO c> \RT\ORD\C.TXT
ECHO x> \RT\ORD\LongNameNumberOne.txt
ECHO d> \RT\ORD\D.TXT
REN \RT\ORD\B.TXT Z.TXT >> %LOG%
DIR \RT\ORD /B >> %LOG%
ECHO --- a long name that fits the old one's slots stays before D.TXT >> %LOG%
REN \RT\ORD\LongNameNumberOne.txt LongNameTwo.txt >> %LOG%
DIR \RT\ORD /B >> %LOG%
ECHO --- a rename that fails keeps the long name: LongNameTwo.txt still >> %LOG%
REN \RT\ORD\LongNameTwo.txt C.TXT >> %LOG%
DIR \RT\ORD /B >> %LOG%
ECHO --- a change of case is allowed, in place, and keeps the alias >> %LOG%
ECHO     (LONGNA~2: the file still wore ~1 when its new alias was chosen) >> %LOG%
REN \RT\ORD\LongNameTwo.txt LONGNAMETWO.TXT >> %LOG%
DIR \RT\ORD /B >> %LOG%
TYPE \RT\ORD\LONGNA~2.TXT >> %LOG%
ECHO --- a long name that needs more slots moves to the end >> %LOG%
REN \RT\ORD\LONGNAMETWO.TXT AVeryMuchLongerNameThanBefore.txt >> %LOG%
DIR \RT\ORD /B >> %LOG%
TYPE \RT\ORD\AVeryMuchLongerNameThanBefore.txt >> %LOG%
ECHO --- and back to a short name, in the slot it moved to >> %LOG%
REN \RT\ORD\AVeryMuchLongerNameThanBefore.txt E.TXT >> %LOG%
DIR \RT\ORD /B >> %LOG%
DEL \RT\ORD\*.TXT
RD \RT\ORD
ECHO --- COPY onto a file, in a batch file: no question >> %LOG%
ECHO first> \RT\SRC1.TXT
ECHO second> \RT\SRC2.TXT
COPY \RT\SRC1.TXT \RT\DST.TXT >> %LOG%
COPY \RT\SRC2.TXT \RT\DST.TXT >> %LOG%
TYPE \RT\DST.TXT >> %LOG%
ECHO --- COPY /-Y, answered No: DST still says second >> %LOG%
COPY /-Y \RT\SRC1.TXT \RT\DST.TXT < \RT\N.ANS >> %LOG%
TYPE \RT\DST.TXT >> %LOG%
ECHO --- COPY /-Y, answered Yes: DST says first >> %LOG%
COPY /-Y \RT\SRC1.TXT \RT\DST.TXT < \RT\Y.ANS >> %LOG%
TYPE \RT\DST.TXT >> %LOG%
ECHO --- COPYCMD=/-Y asks in a batch file, /Y on the line does not >> %LOG%
SET COPYCMD=/-Y
COPY \RT\SRC2.TXT \RT\DST.TXT < \RT\N.ANS >> %LOG%
TYPE \RT\DST.TXT >> %LOG%
COPY /Y \RT\SRC2.TXT \RT\DST.TXT >> %LOG%
TYPE \RT\DST.TXT >> %LOG%
SET COPYCMD=
ECHO --- COPY /-Y to a file that is not there: nothing to ask >> %LOG%
COPY /-Y \RT\SRC1.TXT \RT\NEW.TXT < \RT\N.ANS >> %LOG%
TYPE \RT\NEW.TXT >> %LOG%
ECHO --- tidying up >> %LOG%
DEL \RT\MOVDIR\INSIDE.TXT
RD \RT\MOVDIR
DEL \RT\*.BAK
DEL \RT\*.DOC
DEL \RT\*.TXT
DEL \RT\*.ANS
RD \RT
ECHO === RENTEST: done === >> %LOG%
