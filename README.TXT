PM-DOS - Protected Mode DOS, a 32-bit DOS kernel
================================================

If you can read this with TYPE README.TXT, then the whole file I/O
stack is working:

  INT 21h 3Dh open -> FAT12 root directory search
  INT 21h 3Fh read -> cluster chain walk -> protected-mode to
  real-mode thunk -> BIOS INT 13h -> back again
  INT 21h 40h write -> console driver at B8000h

All in 32-bit flat protected mode, with the BIOS none the wiser.
