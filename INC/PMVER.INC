;=====================================================================
; PMVER.INC - the PM-DOS version, and nothing else.
;
; A file of its own so that the programs which cannot include PMDOS.INC
; can still say which release they belong to: the 16-bit COMMAND.COM
; is a real-mode program, and PMDOS.INC is a 32-bit header full of
; names it must not collide with (wasm folds case across every file it
; reads).  PMDOS.INC includes this, so the number is still written in
; exactly one place - and BUILD.CMD prints the line below before it
; assembles anything.  See the comment above the INCLUDE in PMDOS.INC
; for why that print exists.
;=====================================================================
PMDOS_MAJOR     EQU     0
PMDOS_MINOR     EQU     61
