@ECHO OFF
REM FMTTEST.BAT - FORMAT, on the diskette in B:.  EVERYTHING ON THAT
REM DISKETTE IS LOST, which is the test.
REM
REM It wants a second diskette drive with a disk in it that nobody
REM wants, and it is started from \TESTS, where its two key files are:
REM   CD \TESTS
REM   FMTTEST
REM FORMAT asks for ENTER and for a label, and a batch file cannot
REM press a key, so the answers are typed by a file.  FMT1.KEY is
REM ENTER, the label FMTTEST and ENTER; FMT2.KEY is ENTER and no label.
REM
REM What it covers is each way a diskette can be made:
REM   /U          every track formatted, a new boot sector
REM   (nothing)   the format kept, the surface read
REM   /Q          the format kept, nothing read
REM   /F:720      another capacity, which is every track again
REM   /Q /U       /Q wins: there is a format to keep
REM   /B          taken, and nothing done about it
REM and the five things FORMAT refuses.  A hard disk's side of it - the
REM bad cluster marks /Q keeps and /C tests - needs a volume with marks
REM in it, and no batch file can make one.
REM
REM Every line ends ">> %LOG%", for DIRTEST.BAT's reason.
IF "%LOG%"=="" SET LOG=CON
ECHO === FMTTEST: /U, every track >> %LOG%
FORMAT B: /U < FMT1.KEY >> %LOG%
COPY FMT1.KEY B:\ >> %LOG%
DIR B:\ >> %LOG%
ECHO --- no switch: the format is kept and the surface read >> %LOG%
FORMAT B: < FMT2.KEY >> %LOG%
DIR B:\ >> %LOG%
ECHO --- /Q >> %LOG%
FORMAT B: /Q < FMT1.KEY >> %LOG%
DIR B:\ >> %LOG%
ECHO --- /F:720, and a file on it >> %LOG%
FORMAT B: /F:720 < FMT1.KEY >> %LOG%
COPY FMT1.KEY B:\ >> %LOG%
DIR B:\ >> %LOG%
ECHO --- /U/B/F:1.44MB, glued: back to what the drive is >> %LOG%
FORMAT B:/U/B/F:1.44MB < FMT2.KEY >> %LOG%
DIR B:\ >> %LOG%
ECHO --- /Q /U >> %LOG%
FORMAT B: /Q /U < FMT2.KEY >> %LOG%
ECHO --- refused: /B with /S, a size there is not, >> %LOG%
ECHO --- a size the drive cannot make, a switch, a drive >> %LOG%
FORMAT B: /B /S >> %LOG%
FORMAT B: /F:999 >> %LOG%
FORMAT B: /F:2.88 < FMT2.KEY >> %LOG%
FORMAT B: /X >> %LOG%
FORMAT Q: >> %LOG%
ECHO === FMTTEST DONE === >> %LOG%
