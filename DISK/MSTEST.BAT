@ECHO OFF
REM MSTEST.BAT - the mouse driver, from a DOS box and from a native
REM program.  Run it with CMD /C and its output redirected to a log;
REM every check prints ok or FAILED.
REM
REM The H and C pairs are the two halves of one check: H resets the
REM driver, installs an event handler, shows the cursor and ends without
REM undoing any of it; C then finds that the handler and the cursor went
REM with the program that left them.  The crossed pairs do the same from
REM one kind of program to the other.
ECHO --- MSTEST: a DOS box
MSTEST
ECHO --- MSTEST H, then C
MSTEST H
MSTEST C
ECHO --- MS32TEST: a native program
MS32TEST
ECHO --- MS32TEST H, then C
MS32TEST H
MS32TEST C
ECHO --- a box's leftovers seen from a native program, and back
MSTEST H
MS32TEST C
MS32TEST H
MSTEST C
