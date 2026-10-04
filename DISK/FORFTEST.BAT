@ECHO OFF
REM FORFTEST.BAT - FOR /F over a file, a string and a command, with
REM each of its options; and CMD /C, which the command form runs
REM through.  In \FT on the current drive.
REM
REM Every line ends ">> %LOG%", for DIRTEST.BAT's reason - and inside a
REM FOR the redirection belongs to the body, so it is written there.
IF "%LOG%"=="" SET LOG=CON
ECHO === FORFTEST: making \FT === >> %LOG%
MD \FT
ECHO alpha beta gamma> \FT\F1.TXT
ECHO ; a comment line>> \FT\F1.TXT
ECHO one,two,three>> \FT\F1.TXT
ECHO.>> \FT\F1.TXT
ECHO last line  here>> \FT\F1.TXT
ECHO second file> \FT\F2.TXT
ECHO --- the first token of every line; ; comments and blank lines skipped >> %LOG%
FOR /F %%a IN (\FT\F1.TXT) DO ECHO [%%a] >> %LOG%
ECHO --- tokens=2,3 into a and b >> %LOG%
FOR /F "tokens=2,3" %%a IN (\FT\F1.TXT) DO ECHO [%%a][%%b] >> %LOG%
ECHO --- tokens=1* delims=, : the first, and the rest >> %LOG%
FOR /F "tokens=1* delims=," %%a IN (\FT\F1.TXT) DO ECHO [%%a][%%b] >> %LOG%
ECHO --- skip=2 tokens=*: whole lines, the first two skipped >> %LOG%
FOR /F "skip=2 tokens=*" %%x IN (\FT\F1.TXT) DO ECHO [%%x] >> %LOG%
ECHO --- eol=a: the line starting with a is a comment now, ; is not >> %LOG%
FOR /F "eol=a" %%a IN (\FT\F1.TXT) DO ECHO [%%a] >> %LOG%
ECHO --- tokens=1-3: a range >> %LOG%
FOR /F "tokens=1-3" %%a IN (\FT\F1.TXT) DO ECHO [%%a][%%b][%%c] >> %LOG%
ECHO --- two files, one after the other >> %LOG%
FOR /F "tokens=*" %%a IN (\FT\F1.TXT \FT\F2.TXT) DO ECHO [%%a] >> %LOG%
ECHO --- a string >> %LOG%
FOR /F "tokens=2 delims=," %%a IN ("x,y,z") DO ECHO [%%a] >> %LOG%
ECHO --- a command: what DIR /B prints >> %LOG%
FOR /F %%f IN ('DIR \FT /B /ON') DO ECHO file: %%f >> %LOG%
ECHO --- usebackq: backquotes for the command, quotes for a file >> %LOG%
FOR /F "usebackq tokens=2" %%a IN (`ECHO hello world`) DO ECHO [%%a] >> %LOG%
FOR /F "usebackq" %%a IN ("\FT\F2.TXT") DO ECHO [%%a] >> %LOG%
ECHO --- a file that is not there >> %LOG%
FOR /F %%a IN (\FT\NOSUCH.TXT) DO ECHO [%%a] >> %LOG%
ECHO --- options it does not know >> %LOG%
FOR /F "bogus=1" %%a IN (\FT\F1.TXT) DO ECHO [%%a] >> %LOG%
ECHO --- the classic FOR still works >> %LOG%
FOR %%a IN (x y) DO ECHO [%%a] >> %LOG%
ECHO --- CMD /C >> %LOG%
CMD /C ECHO from a child shell >> %LOG%
CMD /C DIR \FT /B /ON >> %LOG%
ECHO --- tidying up >> %LOG%
DEL \FT\F1.TXT
DEL \FT\F2.TXT
RD \FT
ECHO === FORFTEST: done === >> %LOG%
