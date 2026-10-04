@ECHO OFF
ECHO --- the idiom this was all about ---
REM Before v0.30 the next line created a 25-byte FILE called NUL in
REM whatever directory the batch file was run from, and the DIR after
REM it listed the litter.  It is the commonest line in DOS batch
REM programming and it silently wrote to the disk every time.
VER > NUL
ECHO OK: VER ran with its output discarded
DIR NUL
ECHO (above must say File not found, NOT a NUL entry)
ECHO --- appending to it is just as quiet ---
DIR >> NUL
ECHO OK: a whole DIR went into the bit bucket
ECHO --- and reading from it ends immediately ---
CHOICE /C:YN This should default rather than wait < NUL
ECHO --- other reserved names ---
ECHO test > NUL.TXT
DIR NUL.TXT
ECHO (above must say File not found: the extension is not part of it)
DIR CON
ECHO (above must say File not found too)
ECHO --- a file may not be named one of them ---
ECHO x > CONFIG.NUL
DIR CONFIG.NUL
ECHO (above IS a real file: only the name before the dot counts)
DEL CONFIG.NUL
ECHO NULTEST done
