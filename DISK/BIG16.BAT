@ECHO OFF
ECHO --- BIG16: real 16-bit DOS programs ---
ECHO.
ECHO Every 16-bit guest written for this kernel was written to exercise
ECHO one thing and is a few hundred bytes long.  These are programs
ECHO somebody would actually run: tens of kilobytes, large model,
ECHO hundreds of relocations, and they ask the machine questions.
ECHO.
ECHO step 1: EXEHDR, an .EXE reading an .EXE > C:\BIG16.LOG
EXEHDR C:\TESTS\HELLOX.EXE >> C:\BIG16.LOG
ECHO step 2: the same, through a pipe >> C:\BIG16.LOG
EXEHDR C:\TESTS\HELLOX.EXE | SORT >> C:\BIG16.LOG
ECHO step 3: NMAKE, which EXECs >> C:\BIG16.LOG
NMAKE /? >> C:\BIG16.LOG
ECHO step 4: MSD, which probes the hardware >> C:\BIG16.LOG
MSD /S >> C:\BIG16.LOG
ECHO step 5: MEM, which walks the arena by hand >> C:\BIG16.LOG
MEM622 >> C:\BIG16.LOG
ECHO step 6: got to the end >> C:\BIG16.LOG
ECHO.
TYPE C:\BIG16.LOG
ECHO BIG16 done
