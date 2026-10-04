@ECHO OFF
REM WINDOS.BAT - what WINDRV.EXE starts from inside Windows 3.1: a batch
REM file, so it runs through COMSPEC - the 16-bit COMMAND.COM - on RMDOS,
REM in the DOS session standard mode gives a program.  The last thing it
REM does is make C:\WINDOS.END, which is how WINDRV knows it is over.
REM CRLF.
ECHO === COMMAND.COM in a Windows 3.1 DOS session ===> C:\WINDOS.TXT
VER>> C:\WINDOS.TXT
ECHO COMSPEC=%COMSPEC%>> C:\WINDOS.TXT
ECHO PMDOSTEST=%PMDOSTEST%>> C:\WINDOS.TXT
ECHO --- a directory listing>> C:\WINDOS.TXT
DIR C:\TESTS\A*.COM>> C:\WINDOS.TXT
ECHO --- a 16-bit program and its errorlevel>> C:\WINDOS.TXT
C:\TESTS\ARGS.COM 3>> C:\WINDOS.TXT
IF ERRORLEVEL 4 ECHO WRONG - errorlevel 4>> C:\WINDOS.TXT
IF ERRORLEVEL 3 ECHO errorlevel 3 came back>> C:\WINDOS.TXT
ECHO --- COPY, TYPE, REN and DEL>> C:\WINDOS.TXT
ECHO a file made in Windows> C:\WINTMP1.TXT
COPY C:\WINTMP1.TXT C:\WINTMP2.TXT>> C:\WINDOS.TXT
TYPE C:\WINTMP2.TXT>> C:\WINDOS.TXT
REN C:\WINTMP2.TXT WINTMP3.TXT
IF EXIST C:\WINTMP3.TXT ECHO renamed>> C:\WINDOS.TXT
DEL C:\WINTMP1.TXT
DEL C:\WINTMP3.TXT
IF NOT EXIST C:\WINTMP1.TXT ECHO deleted>> C:\WINDOS.TXT
ECHO --- a pipe, which needs TEMP>> C:\WINDOS.TXT
ECHO through the pipe | C:\TESTS\ARGS.COM 0>> C:\WINDOS.TXT
ECHO --- a second COMMAND.COM>> C:\WINDOS.TXT
COMMAND /C ECHO the inner shell ran>> C:\WINDOS.TXT
ECHO === WINDOS done ===>> C:\WINDOS.TXT
ECHO done> C:\WINDOS.END
