@ECHO OFF
ECHO --- .CMD quoted-argument tests (v0.13) ---
ECHO - a spaced long name born from quoted redirection:
ECHO quoted redirection works > "Quoted Long Name.txt"
DIR QUOTED*.TXT
TYPE "Quoted Long Name.txt"
ECHO - COPY with two quoted arguments:
COPY "Quoted Long Name.txt" "Second Quoted Name.txt"
TYPE "Second Quoted Name.txt"
ECHO - REN with two quoted arguments:
REN "Second Quoted Name.txt" "Third Quoted Name.txt"
TYPE "Third Quoted Name.txt"
ECHO - IF EXIST with quoted long names:
IF EXIST "Quoted Long Name.txt" ECHO exist test passed
IF NOT EXIST "No Such Name Here.txt" ECHO not-exist test passed
ECHO - a quoted FOR item stays one item:
FOR %%F IN ("Quoted Long Name.txt" "Third Quoted Name.txt") DO TYPE %%F
REM THE PATHS ARE SPELT OUT AND THAT IS NOT TIDINESS.  CALL opens what
REM it is given and does not search PATH, so a bare name only works
REM when the sub-batch is in the current directory - and the v0.59 BIN
REM split moved these two to \TESTS while these lines still said the
REM root.  Both checks answered "Bad command or file name" and had not
REM run since, which is the same failure ALLOCTST had and for the same
REM reason.  A test that cannot find its fixture has to SAY so; this
REM one said it and nobody was reading.
ECHO - a quoted argument rides through CALL as one %%1:
CALL \TESTS\SUBQ.CMD "Quoted Long Name.txt"
ECHO - and a .BAT called from here stays strict:
CALL \TESTS\BATQ.BAT
ECHO - clean up:
DEL "Quoted Long Name.txt"
DEL "Third Quoted Name.txt"
DIR QUOTED*.TXT
ECHO CMDTEST done
