@ECHO OFF
REM ====================================================================
REM NESTTEST.BAT - what happens to memory when a nested program dies.
REM
REM EXECTST /F shrinks itself, EXECs a copy with /X, and that copy
REM executes an invalid opcode.  Both of them are two levels inside one
REM DOS box when it happens, so the kernel has to unwind BOTH - an
REM unwind that frees only the program that faulted leaves its parent's
REM block allocated with nothing alive to free it, and conventional
REM memory shrinks a little every time.
REM
REM The two MEM lines either side are the test.  They have to match.
REM ====================================================================
ECHO --- conventional memory before ---
MEM
ECHO --- a fault two levels down (the message below is expected) ---
EXECTST /F
ECHO --- and after: the free figure must be the same ---
MEM
ECHO NESTTEST done
