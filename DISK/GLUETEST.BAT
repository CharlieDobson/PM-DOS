@ECHO OFF
REM GLUETEST.BAT - a command and its argument with no blank between
REM them: DIR/W, CD.., CD\DOS, MEM/F - and a switch in FRONT of a name.
REM
REM All of it is what a real machine said the first time somebody sat
REM down at one: DIR/W was a bad command and DIR /W *.TXT found no
REM files.  Every line ends ">> %LOG%", for DIRTEST.BAT's reason.
REM
REM The fixture, in \GT on the current drive:
REM   A.TXT, B.TXT   plain
REM   SUB\           a directory
REM ARGS.COM has to be on the PATH: it prints the tail it was handed.
IF "%LOG%"=="" SET LOG=CON
ECHO === GLUETEST: making \GT === >> %LOG%
MD\GT
MD\GT\SUB
ECHO one> \GT\A.TXT
ECHO two> \GT\B.TXT
ECHO --- DIR/B: the switch glued on >> %LOG%
DIR/B \GT\*.TXT >> %LOG%
ECHO --- DIR/B/L: and two of them >> %LOG%
DIR/B/L \GT\*.TXT >> %LOG%
ECHO --- DIR /B in front of the name, with a blank >> %LOG%
DIR /B \GT\*.TXT >> %LOG%
ECHO --- DIR /B  /ON   and more blanks than are needed >> %LOG%
DIR /B  /ON   \GT\*.TXT   >> %LOG%
ECHO --- DIR\GT\*.TXT /B: the path glued on >> %LOG%
DIR\GT\*.TXT /B >> %LOG%
ECHO --- CD\GT\SUB, CD.., CD\: where each one lands >> %LOG%
CD\GT\SUB
CD >> %LOG%
CD..
CD >> %LOG%
CD\
CD >> %LOG%
ECHO --- ARGS/X: a program, and its tail has no blank in it >> %LOG%
ARGS/X >> %LOG%
ECHO --- ARGS /X: the same with one >> %LOG%
ARGS /X >> %LOG%
ECHO --- VER/? and DIR/?: help, as before >> %LOG%
VER/? >> %LOG%
ECHO --- DIRX/B and NOSUCH/X: neither is a command >> %LOG%
DIRX/B >> %LOG%
NOSUCH/X >> %LOG%
ECHO --- IF and FOR in front of a glued verb >> %LOG%
IF EXIST \GT\A.TXT DIR/B \GT\A.TXT >> %LOG%
FOR %%F IN (\GT\A.TXT \GT\B.TXT) DO DIR/B %%F >> %LOG%
ECHO --- tidying up >> %LOG%
DEL \GT\A.TXT
DEL \GT\B.TXT
RD\GT\SUB
RD\GT
DIR \GT /B >> %LOG%
ECHO === GLUETEST: done === >> %LOG%
