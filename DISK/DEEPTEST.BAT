@ECHO OFF
REM ====================================================================
REM DEEPTEST.BAT - how deep a directory may be, and what the limit is.
REM
REM Until v0.60 a path was eight components and that was the whole
REM rule.  The bug it produced was not in a deep tree at all: DIR
REM appends "*.*" to whatever it is given, so the LAST directory a
REM user could stand in was the first one they could not look at.
REM
REM   MD \D1\D2\D3\D4\D5\D6\D7\D8   worked
REM   CD \D1\D2\D3\D4\D5\D6\D7\D8   worked
REM   DIR \D1\D2\D3\D4\D5\D6\D7\D8  "Path not found"
REM
REM The depth is 32 now, and the limits that remain are the API's own
REM buffer sizes rather than a count: 64 bytes for 47h's current
REM directory, 128 for 60h's canonical name.  Both are measured from
REM the RENDERING now instead of assumed from the component count -
REM which is why CD into a fifth level works at all, and it did not.
REM ====================================================================
ECHO --- building a tree ten deep ---
MD \D1
MD \D1\D2
MD \D1\D2\D3
MD \D1\D2\D3\D4
MD \D1\D2\D3\D4\D5
MD \D1\D2\D3\D4\D5\D6
MD \D1\D2\D3\D4\D5\D6\D7
MD \D1\D2\D3\D4\D5\D6\D7\D8
MD \D1\D2\D3\D4\D5\D6\D7\D8\D9
MD \D1\D2\D3\D4\D5\D6\D7\D8\D9\D10
COPY \AUTOEXEC.BAT \D1\D2\D3\D4\D5\D6\D7\D8\D9\D10\DEEP.TXT

ECHO --- 1. DIR of the eighth level, which is what used to fail ---
DIR \D1\D2\D3\D4\D5\D6\D7\D8

ECHO --- 2. DIR of the tenth ---
DIR \D1\D2\D3\D4\D5\D6\D7\D8\D9\D10

ECHO --- 3. CD down to it, and the prompt it makes ---
CD \D1\D2\D3\D4\D5\D6\D7\D8\D9\D10
CD
DIR
TYPE DEEP.TXT

ECHO --- 4. and back up the way a program would, with .. ---
CD ..\..\..\..\..
CD
CD \

ECHO --- 5. cleaning up ---
DEL \D1\D2\D3\D4\D5\D6\D7\D8\D9\D10\DEEP.TXT
RD \D1\D2\D3\D4\D5\D6\D7\D8\D9\D10
RD \D1\D2\D3\D4\D5\D6\D7\D8\D9
RD \D1\D2\D3\D4\D5\D6\D7\D8
RD \D1\D2\D3\D4\D5\D6\D7
RD \D1\D2\D3\D4\D5\D6
RD \D1\D2\D3\D4\D5
RD \D1\D2\D3\D4
RD \D1\D2\D3
RD \D1\D2
RD \D1
DIR \D1
ECHO --- DEEPTEST done ---
