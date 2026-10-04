@ECHO OFF
ECHO --- what is on the PCI bus, and does the kernel agree with it ---
PCITEST
IF ERRORLEVEL 1 ECHO FAIL: PCITEST reported a problem
ECHO --- a card's interrupt, in a DOS box ---
VPICTEST
IF ERRORLEVEL 1 ECHO FAIL: VPICTEST reported a problem
ECHO --- the same in protected mode, and a card's memory ---
DPMIIRQ
IF ERRORLEVEL 1 ECHO FAIL: DPMIIRQ reported a problem
ECHO --- and as a 32-bit client, which is what an extender is ---
DPMII32
IF ERRORLEVEL 1 ECHO FAIL: DPMII32 reported a problem
ECHO --- a client's own handlers, and what is there without one ---
DPMIVEC
IF ERRORLEVEL 1 ECHO FAIL: DPMIVEC reported a problem
DPMIV32
IF ERRORLEVEL 1 ECHO FAIL: DPMIV32 reported a problem
ECHO --- where a driver's buffer really is ---
VDSTEST
IF ERRORLEVEL 1 ECHO FAIL: VDSTEST reported a problem
ECHO --- a PCI network card's own interrupt (it wants an 8029) ---
NICIRQ
IF ERRORLEVEL 1 ECHO FAIL: NICIRQ reported a problem
ECHO --- two handlers on one line (it wants a kernel from BUILD irqtest) ---
IRQTEST
IF ERRORLEVEL 1 ECHO FAIL: IRQTEST reported a problem
ECHO BUSTEST done
