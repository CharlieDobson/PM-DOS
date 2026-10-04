@ECHO OFF
REM EDTEST.BAT - EDIT.EXE, driven by key files, in \ED on the current
REM drive (which must not exist beforehand).  Run it with CMD /C and its
REM output redirected to a log.
REM
REM EDIT reads its keys from standard input when EDTEST names a file and
REM input is redirected, and writes its screen into that file wherever
REM the key file asks - TOOLS\MKEDKEYS.PL writes the key files, and its
REM comments say what each session must leave behind.  The screens and
REM the files each session saved are typed into the log after it.
MD \ED
CD \ED
ECHO Hello, world.> ED1.TXT
ECHO Second line>> ED1.TXT
ECHO one cat> ED3.TXT
ECHO two cat>> ED3.TXT
ECHO three cats>> ED3.TXT
ECHO line one> ED7.TXT
ECHO line two>> ED7.TXT
ECHO line three>> ED7.TXT
ECHO line four>> ED7.TXT
COPY C:\TESTS\EDBIN.DAT ED6.BIN > NUL
COPY C:\TESTS\EDTABS.DAT EDTABS.TXT > NUL
COPY C:\TESTS\EDLF.DAT EDLF.TXT > NUL
ECHO alpha> W1.TXT
ECHO beta> W2.TXT
ECHO a> ED11.TXT
ECHO b>> ED11.TXT
ECHO c>> ED11.TXT
ECHO d>> ED11.TXT
ECHO alpha beta gamma> ED14.TXT
ECHO second line here>> ED14.TXT
ECHO third>> ED14.TXT
FOR %%N IN (1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20) DO ECHO line %%N>> ED15.TXT
FOR %%N IN (21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40) DO ECHO line %%N>> ED15.TXT
ECHO one> ED16.TXT
ECHO two>> ED16.TXT
ECHO three>> ED16.TXT

ECHO === ED1: open, type, save through the menu
SET EDTEST=C:\ED\ED1.SCR
EDIT ED1.TXT < C:\TESTS\ED1.KEY
TYPE ED1.SCR
ECHO --- ED1.TXT now:
TYPE ED1.TXT
ECHO.
ECHO --- (end of ED1.TXT)

ECHO === ED2: a new file, indenting, cut and paste, Save As a long name
SET EDTEST=C:\ED\ED2.SCR
EDIT < C:\TESTS\ED2.KEY
TYPE ED2.SCR
ECHO --- Long Name.txt:
TYPE Long Name.txt
ECHO.
ECHO --- (end of Long Name.txt)

ECHO === ED3: Find, F3, Change All, Find and Verify
SET EDTEST=C:\ED\ED3.SCR
EDIT ED3.TXT < C:\TESTS\ED3.KEY
TYPE ED3.SCR
ECHO --- ED3.TXT now:
TYPE ED3.TXT

ECHO === ED4: two windows, Open, F6, the View menu, help, leaving unsaved
SET EDTEST=C:\ED\ED4.SCR
EDIT < C:\TESTS\ED4.KEY
TYPE ED4.SCR
ECHO --- ED1.TXT, which ED4 opened and did not change:
TYPE ED1.TXT
ECHO.
ECHO --- (end of ED1.TXT)

ECHO === ED5: /R, read-only
SET EDTEST=C:\ED\ED5.SCR
EDIT /R ED1.TXT < C:\TESTS\ED5.KEY
TYPE ED5.SCR

ECHO === ED6: /16, a binary file, one byte typed over
SET EDTEST=C:\ED\ED6.SCR
EDIT /16 ED6.BIN < C:\TESTS\ED6.KEY
TYPE ED6.SCR
DIR ED6.BIN

ECHO === ED7: the WordStar keys and a bookmark
SET EDTEST=C:\ED\ED7.SCR
EDIT ED7.TXT < C:\TESTS\ED7.KEY
TYPE ED7.SCR
ECHO --- ED7.TXT now:
TYPE ED7.TXT

ECHO === ED8: /H, and F4
SET EDTEST=C:\ED\ED8.SCR
EDIT /H ED1.TXT < C:\TESTS\ED8.KEY
TYPE ED8.SCR

ECHO === ED9: Settings, Colors, Print, Size Window, Close Window
SET EDTEST=C:\ED\ED9.SCR
EDIT EDTABS.TXT < C:\TESTS\ED9.KEY
TYPE ED9.SCR

ECHO === ED10: two files from a wildcard, the clipboard between them, Close
SET EDTEST=C:\ED\ED10.SCR
EDIT W*.TXT < C:\TESTS\ED10.KEY
TYPE ED10.SCR
ECHO --- W2.TXT now:
TYPE W2.TXT
ECHO.
ECHO --- (end of W2.TXT)

ECHO === ED11: indenting and unindenting lines
SET EDTEST=C:\ED\ED11.SCR
EDIT ED11.TXT < C:\TESTS\ED11.KEY
TYPE ED11.SCR
ECHO --- ED11.TXT now:
TYPE ED11.TXT

ECHO === ED12: LF line ends and a line too long for one
SET EDTEST=C:\ED\ED12.SCR
EDIT EDLF.TXT < C:\TESTS\ED12.KEY
TYPE ED12.SCR
DIR EDLF.TXT

ECHO === ED13: /S
SET EDTEST=C:\ED\ED13.SCR
EDIT /S LONG*.TXT < C:\TESTS\ED13.KEY
TYPE ED13.SCR

ECHO === ED14: the mouse in the text - click, double click, drag, Shift+click
SET EDTEST=C:\ED\ED14.SCR
EDIT ED14.TXT < C:\TESTS\ED14.KEY
TYPE ED14.SCR
ECHO --- ED14.TXT now:
TYPE ED14.TXT

ECHO === ED15: the scroll bars, a dialog and help, by mouse
SET EDTEST=C:\ED\ED15.SCR
EDIT ED15.TXT < C:\TESTS\ED15.KEY
TYPE ED15.SCR

ECHO === ED16: two windows by mouse, and a message box answered with a click
SET EDTEST=C:\ED\ED16.SCR
EDIT ED16.TXT < C:\TESTS\ED16.KEY
TYPE ED16.SCR
ECHO --- ED16.TXT, which must be as it was:
TYPE ED16.TXT

ECHO === ED17: the Open dialog's list of files, by mouse
MD O
ECHO file a> O\A.TXT
ECHO file b> O\B.TXT
SET EDTEST=C:\ED\ED17.SCR
CD O
EDIT < C:\TESTS\ED17.KEY
CD ..
TYPE ED17.SCR

ECHO === EDIT /?
SET EDTEST=
EDIT /?
CD \
ECHO === EDTEST done
