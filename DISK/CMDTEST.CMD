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
ECHO - a quoted argument rides through CALL as one %%1:
CALL SUBQ.CMD "Quoted Long Name.txt"
ECHO - and a .BAT called from here stays strict:
CALL BATQ.BAT
ECHO - clean up:
DEL "Quoted Long Name.txt"
DEL "Third Quoted Name.txt"
DIR QUOTED*.TXT
ECHO CMDTEST done
