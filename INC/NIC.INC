;=====================================================================
; NIC.INC - a NETWORK CARD, as a 32-bit driver presents one
;
; A card driver is a DEVICE= driver (INC\NDRV.INC) with NDTF_NIC in
; its table's flags, and this is what its CALL entry answers: what
; the card is and where, a frame to send, the oldest frame it has
; received, and its counts.  A program reaches the entry through
; 21h/F1h AL=25h NDF_CALL with the driver's row; the stack, when
; there is one, will call it directly.  A frame is an Ethernet frame
; whole - destination, source, type, payload - and nothing here
; knows what is in one: ARP, IP and the rest are the caller's.
;
; WHAT A CARD DRIVER DOES WITH ITS INTERRUPT.  A frame that arrives
; is taken off the card at interrupt time into a queue of the
; driver's own, because a card's buffer is small and a card drops
; what does not fit; NIC_RECV hands over the queue's oldest.  A
; queue that is full drops the newest and counts it.  A frame to send
; goes to the card at once, in the caller's time: the card takes it
; or the call fails, and nothing is kept back to retry.
;
; WHAT A CARD DRIVER DOES AT A PARK.  Windows may run its own driver
; on the same card, so the card is stopped on the way down and set up
; again from nothing on the way back; frames queued before the park
; are still there afterwards, and anything that arrived meanwhile
; was Windows' or nobody's.
;
; Included by a card driver's source and by whatever speaks to one.
; INC\NIC.H is the same for C.
;=====================================================================

;---------------------------------------------------------------------
; THE FUNCTIONS - EBX at the CALL entry, EDI -> the buffer, ECX = its
; length.  Each answers EAX, or CF with AX an error code.
;
;   NIC_INFO    EDI -> NICI_LEN bytes -> EAX = NICI_LEN
;   NIC_SEND    EDI -> a frame of ECX bytes, NIC_MINFRAME to
;               NIC_MAXFRAME -> EAX = ECX.  ERR_BADLEN for any other
;               length; ERR_GENERAL when the card would not take it.
;               A frame shorter than 60 bytes goes out padded with
;               zeros, as the wire wants it
;   NIC_RECV    EDI -> ECX bytes -> EAX = the oldest frame's length,
;               and the frame is there; EAX = 0: nothing is waiting.
;               A buffer shorter than the frame gets its first ECX
;               bytes and the rest is lost, so ECX = NIC_MAXFRAME is
;               what a caller gives.  What the card put after the
;               frame - its own checksum, on some cards - may be in
;               the count: a caller reads the lengths inside a frame,
;               never the count
;   NIC_STATS   EDI -> NICS_LEN bytes -> EAX = NICS_LEN
;---------------------------------------------------------------------
NIC_INFO        EQU     0
NIC_SEND        EQU     1
NIC_RECV        EQU     2
NIC_STATS       EQU     3
NIC_SERVICE     EQU     4               ; drain the card's ring (bridge use)
NIC_WAKE        EQU     5               ; wake the parked card, polled (bridge use)
NIC_WAKE_IRQ    EQU     6               ; wake it with its interrupt live (bridge use)

NIC_HDRLEN      EQU     14              ; destination, source, type
NIC_MINFRAME    EQU     NIC_HDRLEN      ; a header and nothing else
NIC_MAXFRAME    EQU     1514            ; the header and 1500 bytes

;---------------------------------------------------------------------
; NIC_INFO's buffer
;---------------------------------------------------------------------
NICI_MAC        EQU     0               ; 6 bytes: the card's station
                                        ; address, as it is on the wire
NICI_KIND       EQU     6               ; db - NICK_*
NICI_BUS        EQU     7               ; db - NICB_*
NICI_MTU        EQU     8               ; dw - payload bytes a frame
                                        ; carries: 1500
NICI_PORT       EQU     10              ; dw - the first I/O port, or 0
NICI_IRQ        EQU     12              ; db - the line; NIC_NOIRQ when
                                        ; the clock reads the card
NICI_FLAGS      EQU     13              ; db - NICF_*
NICI_SPARE      EQU     14              ; dw - 0
NICI_PCIADDR    EQU     16              ; dd - the function's address as
                                        ; BUS_PCIENUM gives it; 0 on ISA
NICI_NAME       EQU     20              ; 32 bytes, ASCIIZ: what the
                                        ; driver calls the card
NICI_LEN        EQU     52

NICK_ETHERNET   EQU     1

NICB_ISA        EQU     1
NICB_PCI        EQU     2

NICF_POLLED     EQU     01h             ; no interrupt: the clock asks
NICF_WORD       EQU     02h             ; 16-bit transfers to the card

NIC_NOIRQ       EQU     0FFh

;---------------------------------------------------------------------
; NIC_STATS' buffer - dwords, counted since INIT
;---------------------------------------------------------------------
NICS_RXFRAMES   EQU     0               ; frames taken off the card
NICS_RXBYTES    EQU     4
NICS_TXFRAMES   EQU     8               ; frames the card was given
NICS_TXBYTES    EQU     12
NICS_RXDROPPED  EQU     16              ; frames lost for a full queue
NICS_RXERRORS   EQU     20              ; the card's receive errors
NICS_TXERRORS   EQU     24              ; ...and its transmit errors
NICS_OVERFLOWS  EQU     28              ; the card's buffer ran over
NICS_IRQS       EQU     32              ; interrupts that were the card's
NICS_QUEUED     EQU     36              ; frames waiting now
NICS_LEN        EQU     40
