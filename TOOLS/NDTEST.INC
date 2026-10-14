;=====================================================================
; NDTEST.INC - what NDTEST.SYS reports and NDRVTEST.EXE reads
;
; The buffer NDRVTEST hands NDTEST through 21h/F1h AL=25h NDF_CALL,
; function NDC_REPORT.  Two programs agree on it and nothing else
; does, so it lives here and not in INC\NDRV.INC.
;=====================================================================
NDC_REPORT      EQU     1               ; EBX: fill the buffer

NDTR_LINKED     EQU     0               ; dd - the table's address as a
                                        ; dword the linker wrote and the
                                        ; loader relocated
NDTR_RUNNING    EQU     4               ; dd - ...and as the code finds it
NDTR_HDRBASE    EQU     8               ; dd - D32H_BASE in the header now
NDTR_HDRENTRY   EQU     12              ; dd - D32H_ENTRY in the header now
NDTR_NRELOC     EQU     16              ; dd - D32H_NRELOC: how many moved
NDTR_SERVICES   EQU     20              ; dd - ESI at INIT
NDTR_ROW        EQU     24              ; dd - EDI at INIT
NDTR_BLOCK      EQU     28              ; dd - what NDS_ALLOC gave, or 0
NDTR_BLOCKOK    EQU     32              ; dd - 1: the pattern read back
NDTR_TICKS      EQU     36              ; dd - tick hook calls so far
NDTR_TICKHOOK   EQU     40              ; dd - 0: hooked, 1: refused
NDTR_PARKS      EQU     44              ; dd - PARK calls with AL = 0
NDTR_UNPARKS    EQU     48              ; dd - ...and with AL = 1
NDTR_HALTS      EQU     52              ; dd - HALT calls
NDTR_IRQ        EQU     56              ; dd - the line /I: named, or -1
NDTR_IRQHOOK    EQU     60              ; dd - 0: hooked, 1: refused,
                                        ; 2: not asked
NDTR_IRQS       EQU     64              ; dd - interrupts seen on it
NDTR_CALLS      EQU     68              ; dd - CALL entries so far, this
                                        ; one included
NDTR_LINE       EQU     72              ; 80 bytes - the DEVICE= line as
                                        ; INIT was given it, ASCIIZ
NDTR_LEN        EQU     152
