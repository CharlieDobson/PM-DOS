@ECHO OFF
REM HLPTEST.BAT - HELP.EXE, driven by key files, in \HLPT on the current
REM drive (which must not exist beforehand).  Run it with CMD /C and its
REM output redirected to a log.  HELP.HLP has to be where HELP finds it:
REM the current directory, beside HELP.EXE, or along the PATH.
REM
REM HELP reads its keys from standard input when HLPTEST names a file and
REM input is redirected, and writes its screen into that file wherever
REM the key file asks - TOOLS\MKEDKEYS.PL writes the key files, and its
REM comments say what each session does.  The screens are the help
REM file's text, so this log changes whenever HELP.HLP does.
MD \HLPT
CD \HLPT

ECHO === HELP /?
HELP /?

ECHO === HL1: the contents, a topic, the menus and dialogs, the mouse
SET HLPTEST=C:\HLPT\HL1.SCR
HELP < C:\TESTS\HL1.KEY
TYPE HL1.SCR

ECHO === HL2: HELP DIR - frozen lines, Examples, Find, F3, the help box
SET HLPTEST=C:\HLPT\HL2.SCR
HELP DIR < C:\TESTS\HL2.KEY
TYPE HL2.SCR

ECHO === HL3: HELP NOSUCH - the message, then the contents
SET HLPTEST=C:\HLPT\HL3.SCR
HELP NOSUCH < C:\TESTS\HL3.KEY
TYPE HL3.SCR

ECHO === HELP with no help file: the message and errorlevel 1
SET HLPTEST=
COPY C:\HELP.EXE \HLPT\NOHELP.EXE > NUL
MD \HLPT\EMPTY
CD \HLPT\EMPTY
SET SAVEPATH=%PATH%
PATH \HLPT\EMPTY
..\NOHELP.EXE
IF ERRORLEVEL 1 ECHO errorlevel 1
PATH %SAVEPATH%
SET SAVEPATH=
CD \
ECHO === HLPTEST done
