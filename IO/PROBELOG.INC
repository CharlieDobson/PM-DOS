;=====================================================================
; PROBELOG.INC - what the boot-time probes saw
;
; A DEVICE THAT WAS NOT FOUND LEAVES NOTHING BEHIND TO ASK ABOUT.  The
; mouse driver keeps a type and the ATA driver keeps a table, and both
; are empty for a device that did not answer - so "no mouse" on a
; machine with a mouse in it said nothing about which of eight steps
; had gone wrong, and the only way to find out was a build with the
; trace compiled in and a second floppy to carry it.
;
; So the probes write down what they read as they read it: the bytes
; the keyboard controller and the mouse answered with, and for each of
; the eight IDE positions the status register at every step and the
; signature the drive left, and what ISA Plug and Play found and where
; it put it.  PLOG_LEN bytes, laid out by the PLM_, PLI_ and PLP_
; equates in PMDOS.INC, and \TESTS\HWPROBE prints them.
;
; It is a record and nothing reads it to make a decision.
;
; Included by IOPM.ASM, before MOUSE.INC and ATA.INC.
;=====================================================================

;---------------------------------------------------------------------
; plog_get - IOS_PROBELOG.  EDI -> PLOG_LEN bytes.  All registers kept.
;---------------------------------------------------------------------
plog_get:
        push    ecx
        push    esi
        push    edi
        mov     esi,OFFSET plog
        mov     ecx,PLOG_LEN / 4
        cld
        rep     movsd
        pop     edi
        pop     esi
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; plog_ide - EBX = a device's place in the table -> EDI = its record.
;---------------------------------------------------------------------
plog_ide:
        mov     edi,ebx
        imul    edi,PLI_LEN
        add     edi,OFFSET plog + PLI_FIRST
        ret

plog    db      PLOG_LEN dup(0)
