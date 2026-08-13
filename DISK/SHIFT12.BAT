@ECHO OFF
ECHO -- twelve arguments, only ten nameable at a time --
ECHO %1 %2 %3 %4 %5 %6 %7 %8 %9
SHIFT
SHIFT
ECHO after two SHIFTs, %%9 reaches the twelfth:
ECHO %1 %2 %3 %4 %5 %6 %7 %8 %9
