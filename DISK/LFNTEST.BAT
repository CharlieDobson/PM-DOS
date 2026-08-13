@ECHO OFF
ECHO --- LFN read tests (v0.11) ---
ECHO - DIR shows the long names:
DIR *.TXT
ECHO - open by the 8.3 alias:
TYPE LONGFI~1.TXT
ECHO - open by the long name (no spaces):
TYPE MixedCaseName.txt
ECHO - open by the long name (spaces and all):
TYPE Long File Name.txt
ECHO - walk through a long-named directory:
TYPE Long Directory\NESTED.TXT
CD Long Directory
DIR
CD ..
ECHO --- LFN write tests (v0.12) ---
ECHO - create long names by redirection:
ECHO The first created file. > CreatedFileNumberOne.txt
ECHO The second created file. > CreatedFileNumberTwo.txt
DIR CREATE*.TXT
ECHO - the aliases got distinct tails:
TYPE CREATE~1.TXT
TYPE CREATE~2.TXT
ECHO - copy to a long name:
COPY CREATE~1.TXT CopiedLongTestFile.txt
TYPE CopiedLongTestFile.txt
ECHO - rename to a long name:
REN CopiedLongTestFile.txt RenamedLongTestFile.txt
TYPE RenamedLongTestFile.txt
ECHO - a long-named directory, made, entered and removed:
MD Created Long Directory
CD Created Long Directory
CD
CD ..
RD Created Long Directory
ECHO - delete the long names (chains go with them):
DEL CreatedFileNumberOne.txt
DEL CreatedFileNumberTwo.txt
DEL RenamedLongTestFile.txt
DIR CREATE*.TXT
ECHO LFNTEST done
