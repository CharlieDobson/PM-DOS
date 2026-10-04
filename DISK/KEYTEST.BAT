@ECHO OFF
REM KEYTEST.BAT - DOSKEY: the command, and the editor.
REM
REM THE COMMAND is tested the ordinary way.  THE EDITOR only runs at a
REM prompt with a keyboard behind it, so its keystrokes come from a
REM file instead: KEYTEST.KEY, made by TOOLS\MKKEYS.PL, holds them as
REM the bytes 21h/07h returns - a zero and a scan code for Up, F8,
REM Ctrl+Left and the rest - and KYTEST=1 tells the editor to take its
REM keys from a redirected standard input, which it otherwise refuses.
REM Each line it types writes a file, and what the files say afterwards
REM is what the editor made of the keys.  MKKEYS.PL says what each one
REM must hold.
REM
REM KEYFILE names the key file; it is \TESTS\KEYTEST.KEY on the images.
REM Every line ends ">> %LOG%", for DIRTEST.BAT's reason.
IF "%LOG%"=="" SET LOG=CON
IF "%KEYFILE%"=="" SET KEYFILE=\TESTS\KEYTEST.KEY
ECHO === KEYTEST === >> %LOG%
ECHO --- two macros, listed back with their $ codes >> %LOG%
DOSKEY ls=DIR $1 /B /ON $T ECHO listed $*
DOSKEY gt=ECHO a$Gb$Lc$Bd costs $$5
DOSKEY /M >> %LOG%
ECHO --- ls deleted, gt still there >> %LOG%
DOSKEY ls=
DOSKEY /MACROS >> %LOG%
ECHO --- a macro may be redefined >> %LOG%
DOSKEY gt=ECHO redefined
DOSKEY /M >> %LOG%
ECHO --- /BUFSIZE is clamped, not refused >> %LOG%
DOSKEY /BUFSIZE=100 >> %LOG%
DOSKEY /BUFSIZE=99999 >> %LOG%
DOSKEY /BUFSIZE=1024 /M >> %LOG%
ECHO --- a switch it does not know >> %LOG%
DOSKEY /NOSUCH >> %LOG%
ECHO --- a definition with no name >> %LOG%
DOSKEY =ECHO nameless >> %LOG%
ECHO --- /REINSTALL forgets them all >> %LOG%
DOSKEY /REINSTALL
DOSKEY /M >> %LOG%
ECHO --- the editor, typed from %KEYFILE% into a shell of its own >> %LOG%
MD \KT
CD \KT
SET KYTEST=1
CMD < %KEYFILE%
SET KYTEST=
ECHO K1 one, K2 one, K3 two, K4 new two, K5 two, K6 six, K7 six: >> %LOG%
TYPE K1.TXT >> %LOG%
TYPE K2.TXT >> %LOG%
TYPE K3.TXT >> %LOG%
TYPE K4.TXT >> %LOG%
TYPE K5.TXT >> %LOG%
TYPE K6.TXT >> %LOG%
TYPE K7.TXT >> %LOG%
ECHO K8 eight, K9 ninety, KA one, KB new two, KC two, KD two: >> %LOG%
TYPE K8.TXT >> %LOG%
TYPE K9.TXT >> %LOG%
TYPE KA.TXT >> %LOG%
TYPE KB.TXT >> %LOG%
TYPE KC.TXT >> %LOG%
TYPE KD.TXT >> %LOG%
ECHO KE fifteen, KF sixteen, KG seventeen, KH eighteen: >> %LOG%
TYPE KE.TXT >> %LOG%
TYPE KF.TXT >> %LOG%
TYPE KG.TXT >> %LOG%
TYPE KH.TXT >> %LOG%
ECHO KI alpha-beta, KJ first, KK second, KL [a b  c], KM t1, KN t2: >> %LOG%
TYPE KI.TXT >> %LOG%
TYPE KJ.TXT >> %LOG%
TYPE KK.TXT >> %LOG%
TYPE KL.TXT >> %LOG%
TYPE KM.TXT >> %LOG%
TYPE KN.TXT >> %LOG%
ECHO KO after f7, KQ q: >> %LOG%
TYPE KO.TXT >> %LOG%
TYPE KQ.TXT >> %LOG%
ECHO --- DOSKEY /H, and /M, as the editor's shell saw them >> %LOG%
TYPE HIST.TXT >> %LOG%
TYPE MACS.TXT >> %LOG%
ECHO --- after Alt+F10 there are no macros, after Alt+F7 one line >> %LOG%
TYPE MACS2.TXT >> %LOG%
TYPE HIST2.TXT >> %LOG%
DEL \KT\*.TXT
CD \
RD \KT
ECHO === KEYTEST: done === >> %LOG%
