@ECHO OFF
REM ====================================================================
REM HELPTEST.BAT - /? on everything that has a name you can type.
REM
REM WHAT IT IS ACTUALLY ASKING.  A help screen is easy to add and easy
REM to add WRONGLY, and the two failures look nothing alike:
REM
REM   1  a command that does not answer /? at all - it runs instead,
REM      and for FORMAT or DEL that is not a harmless mistake
REM   2  a command that answers /? when it should have got on with the
REM      job, which is the ECHO and REM case: their argument is text,
REM      and text is allowed to contain a slash and a question mark
REM
REM So the sections below are not one list.  Section 1 is "every verb
REM says something"; section 2 is "the ones whose argument is text say
REM it only when /? stands alone"; section 3 is the external commands,
REM which each parse their own tail and so are each a separate chance
REM to have forgotten.
REM
REM WHY IT IS LONG AND WHY THAT IS FINE: reading it is the test.  Every
REM line of help here describes THIS shell, so a switch that appears on
REM a screen below and does nothing when typed is a bug in one or the
REM other, and this is where the two are put side by side.
REM ====================================================================

ECHO ==================== 1. INTERNAL COMMANDS ====================
ECHO --- DIR ---
DIR /?
ECHO --- TYPE ---
TYPE /?
ECHO --- COPY ---
COPY /?
ECHO --- DEL ---
DEL /?
ECHO --- ERASE (the same help as DEL) ---
ERASE /?
ECHO --- REN ---
REN /?
ECHO --- RENAME ---
RENAME /?
ECHO --- CLS ---
CLS /?
ECHO --- VER ---
VER /?
ECHO --- VOL ---
VOL /?
ECHO --- DATE ---
DATE /?
ECHO --- TIME ---
TIME /?
ECHO --- PAUSE ---
PAUSE /?
ECHO --- REM ---
REM /?
ECHO --- EXIT ---
EXIT /?
ECHO --- ECHO ---
ECHO /?
ECHO --- BREAK ---
BREAK /?
ECHO --- VERIFY ---
VERIFY /?
ECHO --- SET ---
SET /?
ECHO --- PATH ---
PATH /?
ECHO --- PROMPT ---
PROMPT /?
ECHO --- MD ---
MD /?
ECHO --- MKDIR ---
MKDIR /?
ECHO --- CD ---
CD /?
ECHO --- CHDIR ---
CHDIR /?
ECHO --- RD ---
RD /?
ECHO --- RMDIR ---
RMDIR /?
ECHO --- GOTO ---
GOTO /?
ECHO --- IF ---
IF /?
ECHO --- FOR ---
FOR /?
ECHO --- SHIFT ---
SHIFT /?
ECHO --- CALL ---
CALL /?
ECHO --- CTTY ---
CTTY /?
ECHO --- DOSKEY ---
DOSKEY /?
ECHO --- LH ---
LH /?
ECHO --- LOADHIGH ---
LOADHIGH /?

ECHO ==================== 2. THE TWO RULES ====================
REM
REM A command whose argument is TEXT answers /? only when /? is the
REM whole of that argument.  Anything else and the text wins: the
REM three lines below must PRINT THEIR WORDS and not a help screen.
ECHO --- ECHO with /? inside a message: expect the message ---
ECHO Run SETUP /? for help
ECHO --- ECHO with /? at the end: expect the message ---
ECHO see COPY /?
ECHO --- REM with /? inside a comment: expect NOTHING at all ---
REM this is a comment mentioning /? and must stay silent
ECHO --- SET with /? as a value: expect no help, and HELPV=/? below ---
SET HELPV=/?
SET
SET HELPV=
ECHO --- PROMPT is in the same group; the prompt is unchanged ---
PROMPT $P$G
REM
REM And a command whose argument is a FILE answers /? wherever it is,
REM because nothing else it could be is a file.
ECHO --- DIR with a pattern AND /?: expect DIR help, not a listing ---
DIR *.SYS /?
ECHO --- the glued form, which the verb scanner has to split ---
DIR/?
ECHO --- /?? is a PATTERN and not the switch: expect no help screen ---
DIR /??
ECHO --- and the same verbs without /? still work ---
VER
VOL
CD

ECHO ==================== 3. EXTERNAL COMMANDS ====================
ECHO --- MEM ---
MEM /?
ECHO --- MORE ---
MORE /?
ECHO --- SORT ---
SORT /?
ECHO --- CHOICE (must NOT wait for a key) ---
CHOICE /?
ECHO --- FORMAT (must NOT format anything) ---
FORMAT /?
ECHO --- FORMAT with a drive AND /?: still help, still no format ---
FORMAT A: /?
ECHO --- SYS (must NOT write a boot sector) ---
SYS /?
ECHO --- FDISK (must NOT touch a partition table) ---
FDISK /?
ECHO --- MOVE ---
MOVE /?
ECHO --- MOUNT ---
MOUNT /?
ECHO --- CACHE ---
CACHE /?
ECHO --- UNDELETE (must NOT recover or purge anything) ---
UNDELETE /PURGE /?
ECHO --- CDEX ---
CDEX /?
ECHO --- ISAPNP (must NOT switch anything on) ---
ISAPNP /A /?
ECHO --- SHUTDOWN (must NOT write anything or switch the computer off) ---
SHUTDOWN /?
ECHO --- SHUTDOWN with something else AND /?: still help, still running ---
SHUTDOWN NOW /?
ECHO --- FC (must NOT compare anything) ---
FC /?
ECHO --- FC with two names AND /?: still help, no comparison ---
FC A.TXT B.TXT /?
ECHO --- FIND ---
FIND /?
ECHO --- FIND with a string AND a file AND /?: still help, no search ---
FIND "x" A.TXT /?
ECHO --- DISKCOPY (must NOT ask for a diskette) ---
DISKCOPY /?
ECHO --- DISKCOPY with drives AND /?: still help, still no copy ---
DISKCOPY A: B: /?
ECHO --- DISKCOMP (must NOT ask for a diskette) ---
DISKCOMP /?
ECHO --- ATTRIB (must NOT change an attribute, and must not list) ---
ATTRIB /?
ECHO --- ATTRIB with a change AND /?: still help, still no change ---
ATTRIB +R *.SYS /?
ECHO --- CHKDSK (must NOT check a disk) ---
CHKDSK /?
ECHO --- CHKDSK with a drive AND /F AND /?: still help, still no fix ---
CHKDSK C: /F /?
ECHO --- LABEL (must NOT show a label or ask for one) ---
LABEL /?
ECHO --- LABEL with a drive AND a label AND /?: still help, no label ---
LABEL C:NEWNAME /?
ECHO --- DRIVES ---
\TESTS\DRIVES /?
ECHO --- HWPROBE ---
\TESTS\HWPROBE /?
ECHO --- CMD itself, which is a program like any other ---
CMD /?
ECHO --- COMMAND, the 16-bit shell: its help is read back out of its file ---
COMMAND /?

ECHO ==================== HELPTEST DONE ====================
