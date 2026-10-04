;=====================================================================
; HELPTEXT.INC - what /? prints, one text per command.
;
; NOT IN MEMORY.  These sit past resident_end, where the stack is laid
; over them once the shell is running; show_help reads the one it
; wants back out of the file.  That is a few hundred bytes of disk
; reading on a /?, against ten kilobytes of every DOS box's memory.
;
; Each text is hl_NAME to hl_NAME_end, plain text with CR LF, and
; int_table records both ends.
;=====================================================================
hl_command  db  'Starts a new copy of the PM-DOS 16-bit command interpreter.',13,10
            db  13,10
            db  'COMMAND [[drive:]path] [device] [/E:nnnnn] [/P [/D]] [/F]',13,10
            db  '        [/C command | /K command]',13,10
            db  13,10
            db  '  [drive:]path    The directory COMMAND.COM is in (COMSPEC, with /P).',13,10
            db  '  device          The device to use for input and output.',13,10
            db  '  /E:nnnnn        The size of the environment, 160 to 32768 bytes.',13,10
            db  '  /P              Permanent: runs AUTOEXEC.BAT, and EXIT does not end it.',13,10
            db  '  /D              With /P: skips AUTOEXEC.BAT, and the date and time.',13,10
            db  '  /F              Answers Fail to every critical error, without asking.',13,10
            db  '  /C command      Runs the command and returns.',13,10
            db  '  /K command      Runs the command and carries on.',13,10
            db  13,10
            db  'This is the interpreter DOS programs get - COMSPEC names it. PM-DOS''s own',13,10
            db  'command interpreter is CMD.EXE.',13,10
hl_command_end LABEL BYTE

hl_break    db  'Sets or clears extended CTRL+C checking.',13,10
            db  13,10
            db  'BREAK [ON | OFF]',13,10
            db  13,10
            db  'Type BREAK without a parameter to display the current BREAK setting.',13,10
hl_break_end LABEL BYTE

hl_call     db  'Calls one batch program from another.',13,10
            db  13,10
            db  'CALL [drive:][path]filename [batch-parameters]',13,10
            db  13,10
            db  '  batch-parameters   The command-line information the batch program needs.',13,10
hl_call_end LABEL BYTE

hl_cd       db  'Displays the name of or changes the current directory.',13,10
            db  13,10
            db  'CHDIR [drive:][path]',13,10
            db  'CHDIR[..]',13,10
            db  'CD [drive:][path]',13,10
            db  'CD[..]',13,10
            db  13,10
            db  '  ..   Specifies that you want to change to the parent directory.',13,10
            db  13,10
            db  'Type CD drive: to display the current directory in the specified drive.',13,10
            db  'Type CD without parameters to display the current drive and directory.',13,10
hl_cd_end   LABEL BYTE

hl_chcp     db  'Displays or sets the active code page number.',13,10
            db  13,10
            db  'CHCP [nnn]',13,10
            db  13,10
            db  '  nnn   Specifies a code page number.',13,10
            db  13,10
            db  'Type CHCP without a parameter to display the active code page number.',13,10
hl_chcp_end LABEL BYTE

hl_cls      db  'Clears the screen.',13,10
            db  13,10
            db  'CLS',13,10
hl_cls_end  LABEL BYTE

hl_copy     db  'Copies one or more files to another location.',13,10
            db  13,10
            db  'COPY [/A | /B] source [/A | /B] [+ source [/A | /B] [+ ...]] [destination',13,10
            db  '  [/A | /B]] [/V] [/Y | /-Y]',13,10
            db  13,10
            db  '  source       Specifies the file or files to be copied.',13,10
            db  '  /A           Indicates an ASCII text file.',13,10
            db  '  /B           Indicates a binary file.',13,10
            db  '  destination  Specifies the directory and/or filename for the new file(s).',13,10
            db  '  /V           Verifies that new files are written correctly.',13,10
            db  '  /Y           Replaces existing files without asking.',13,10
            db  '  /-Y          Asks before replacing an existing file.',13,10
            db  13,10
            db  'COPYCMD may hold /Y or /-Y.  At the prompt COPY asks before replacing a file;',13,10
            db  'in a batch file it does not.  To append files, give one file for the',13,10
            db  'destination and several for the source (wildcards or file1+file2+file3).',13,10
hl_copy_end LABEL BYTE

hl_ctty     db  'Changes the terminal device used to control your system.',13,10
            db  13,10
            db  'CTTY device',13,10
            db  13,10
            db  '  device   The terminal device to use, such as COM1.',13,10
hl_ctty_end LABEL BYTE

hl_date     db  'Displays or sets the date.',13,10
            db  13,10
            db  'DATE [date]',13,10
            db  13,10
            db  'Type DATE without parameters to display the current date setting and',13,10
            db  'a prompt for a new one.  Press ENTER to keep the same date.',13,10
hl_date_end LABEL BYTE

hl_del      db  'Deletes one or more files.',13,10
            db  13,10
            db  'DEL [drive:][path]filename [/P]',13,10
            db  'ERASE [drive:][path]filename [/P]',13,10
            db  13,10
            db  '  [drive:][path]filename  Specifies the file(s) to delete.  Specify multiple',13,10
            db  '                          files by using wildcards.',13,10
            db  '  /P                      Prompts for confirmation before deleting each file.',13,10
hl_del_end  LABEL BYTE

hl_dir      db  'Displays a list of files and subdirectories in a directory.',13,10
            db  13,10
            db  'DIR [drive:][path][filename] [/P] [/W] [/A[[:]attributes]]',13,10
            db  '  [/O[[:]sortorder]] [/S] [/B] [/L]',13,10
            db  13,10
            db  '  [drive:][path][filename]',13,10
            db  '              Specifies drive, directory, and/or files to list.',13,10
            db  '  /P          Pauses after each screenful of information.',13,10
            db  '  /W          Uses wide list format.',13,10
            db  '  /A          Displays files with specified attributes.',13,10
            db  '  attributes   D  Directories                R  Read-only files',13,10
            db  '               H  Hidden files               A  Files ready for archiving',13,10
            db  '               S  System files               -  Prefix meaning "not"',13,10
            db  '  /O          List by files in sorted order.',13,10
            db  '  sortorder    N  By name (alphabetic)       S  By size (smallest first)',13,10
            db  '               E  By extension (alphabetic)  D  By date & time (earliest first)',13,10
            db  '               G  Group directories first    -  Prefix to reverse order',13,10
            db  '  /S          Displays files in specified directory and all subdirectories.',13,10
            db  '  /B          Uses bare format (no heading information or summary).',13,10
            db  '  /L          Uses lowercase.',13,10
            db  13,10
            db  'Switches may be preset in the DIRCMD environment variable.  Override',13,10
            db  'preset switches by prefixing any switch with - (hyphen)--for example, /-W.',13,10
hl_dir_end  LABEL BYTE

hl_echo     db  'Displays messages, or turns command-echoing on or off.',13,10
            db  13,10
            db  '  ECHO [ON | OFF]',13,10
            db  '  ECHO [message]',13,10
            db  13,10
            db  'Type ECHO without parameters to display the current echo setting.',13,10
hl_echo_end LABEL BYTE

hl_exit     db  'Quits the COMMAND.COM program (command interpreter).',13,10
            db  13,10
            db  'EXIT',13,10
hl_exit_end LABEL BYTE

hl_for      db  'Runs a specified command for each file in a set of files.',13,10
            db  13,10
            db  'FOR %variable IN (set) DO command [command-parameters]',13,10
            db  13,10
            db  '  %variable  Specifies a replaceable parameter.',13,10
            db  '  (set)      Specifies a set of one or more files.  Wildcards may be used.',13,10
            db  '  command    Specifies the command to carry out for each file.',13,10
            db  '  command-parameters',13,10
            db  '             Specifies parameters or switches for the specified command.',13,10
            db  13,10
            db  'To use the FOR command in a batch program, specify %%variable instead of',13,10
            db  '%variable.',13,10
hl_for_end  LABEL BYTE

hl_goto     db  'Directs MS-DOS to a labelled line in a batch program.',13,10
            db  13,10
            db  'GOTO label',13,10
            db  13,10
            db  '  label   Specifies a text string used in the batch program as a label.',13,10
            db  13,10
            db  'You type a label on a line by itself, beginning with a colon.',13,10
hl_goto_end LABEL BYTE

hl_if       db  'Performs conditional processing in batch programs.',13,10
            db  13,10
            db  'IF [NOT] ERRORLEVEL number command',13,10
            db  'IF [NOT] string1==string2 command',13,10
            db  'IF [NOT] EXIST filename command',13,10
            db  13,10
            db  '  NOT               Specifies that the condition is to be reversed.',13,10
            db  '  ERRORLEVEL number Specifies a true condition if the last program run',13,10
            db  '                    returned an exit code equal to or greater than the',13,10
            db  '                    number specified.',13,10
            db  '  command           Specifies the command to carry out if the condition is',13,10
            db  '                    met.',13,10
            db  '  string1==string2  Specifies a true condition if the specified text strings',13,10
            db  '                    match.',13,10
            db  '  EXIST filename    Specifies a true condition if the specified filename',13,10
            db  '                    exists.',13,10
hl_if_end   LABEL BYTE

hl_lh       db  'Loads a program into the upper memory area.',13,10
            db  13,10
            db  'LOADHIGH [drive:][path]filename [parameters]',13,10
            db  'LH [drive:][path]filename [parameters]',13,10
            db  13,10
            db  '  parameters   Specifies any command-line information the program needs.',13,10
            db  13,10
            db  '/L and /S are accepted: PM-DOS has one upper memory pool.',13,10
hl_lh_end   LABEL BYTE

hl_md       db  'Creates a directory.',13,10
            db  13,10
            db  'MKDIR [drive:]path',13,10
            db  'MD [drive:]path',13,10
hl_md_end   LABEL BYTE

hl_path     db  'Displays or sets a search path for executable files.',13,10
            db  13,10
            db  'PATH [[drive:]path[;...]]',13,10
            db  'PATH ;',13,10
            db  13,10
            db  'Type PATH ; to clear all search-path settings and direct MS-DOS to search',13,10
            db  'only in the current directory.',13,10
            db  'Type PATH without parameters to display the current path.',13,10
hl_path_end LABEL BYTE

hl_pause    db  'Suspends processing of a batch program and displays the message',13,10
            db  '"Press any key to continue...."',13,10
            db  13,10
            db  'PAUSE',13,10
hl_pause_end LABEL BYTE

hl_prompt   db  'Changes the MS-DOS command prompt.',13,10
            db  13,10
            db  'PROMPT [text]',13,10
            db  13,10
            db  '  text    Specifies a new command prompt.',13,10
            db  13,10
            db  'Prompt can be made up of normal characters and the following special codes:',13,10
            db  13,10
            db  '  $Q   = (equal sign)',13,10
            db  '  $$   $ (dollar sign)',13,10
            db  '  $T   Current time',13,10
            db  '  $D   Current date',13,10
            db  '  $P   Current drive and path',13,10
            db  '  $V   PM-DOS version number',13,10
            db  '  $N   Current drive',13,10
            db  '  $G   > (greater-than sign)',13,10
            db  '  $L   < (less-than sign)',13,10
            db  '  $B   | (pipe)',13,10
            db  '  $H   Backspace (erases previous character)',13,10
            db  '  $E   Escape code (ASCII code 27)',13,10
            db  '  $_   Carriage return and linefeed',13,10
            db  13,10
            db  'Type PROMPT without parameters to reset the prompt to the default setting.',13,10
hl_prompt_end LABEL BYTE

hl_rd       db  'Removes (deletes) a directory.',13,10
            db  13,10
            db  'RMDIR [drive:]path',13,10
            db  'RD [drive:]path',13,10
hl_rd_end   LABEL BYTE

hl_rem      db  'Records comments (remarks) in a batch file or CONFIG.SYS.',13,10
            db  13,10
            db  'REM [comment]',13,10
hl_rem_end  LABEL BYTE

hl_ren      db  'Renames a file/directory or files/directories.',13,10
            db  13,10
            db  'RENAME [drive:][path][directoryname1 | filename1] [directoryname2 | filename2]',13,10
            db  'REN [drive:][path][directoryname1 | filename1] [directoryname2 | filename2]',13,10
            db  13,10
            db  'Note that you cannot specify a new drive or path for your destination.',13,10
hl_ren_end  LABEL BYTE

hl_set      db  'Displays, sets, or removes MS-DOS environment variables.',13,10
            db  13,10
            db  'SET [variable=[string]]',13,10
            db  13,10
            db  '  variable  Specifies the environment-variable name.',13,10
            db  '  string    Specifies a series of characters to assign to the variable.',13,10
            db  13,10
            db  'Type SET without parameters to display the current environment variables.',13,10
hl_set_end  LABEL BYTE

hl_shift    db  'Changes the position of replaceable parameters in a batch file.',13,10
            db  13,10
            db  'SHIFT',13,10
hl_shift_end LABEL BYTE

hl_time     db  'Displays or sets the system time.',13,10
            db  13,10
            db  'TIME [time]',13,10
            db  13,10
            db  'Type TIME with no parameters to display the current time setting and a prompt',13,10
            db  'for a new one.  Press ENTER to keep the same time.',13,10
hl_time_end LABEL BYTE

hl_truename db  'Displays the full name DOS gives a file or directory.',13,10
            db  13,10
            db  'TRUENAME [drive:][path][filename]',13,10
hl_truename_end LABEL BYTE

hl_type     db  'Displays the contents of a text file.',13,10
            db  13,10
            db  'TYPE [drive:][path]filename',13,10
hl_type_end LABEL BYTE

hl_ver      db  'Displays the PM-DOS version.',13,10
            db  13,10
            db  'VER',13,10
hl_ver_end  LABEL BYTE

hl_verify   db  'Tells MS-DOS whether to verify that your files are written correctly to a',13,10
            db  'disk.',13,10
            db  13,10
            db  'VERIFY [ON | OFF]',13,10
            db  13,10
            db  'Type VERIFY without a parameter to display the current VERIFY setting.',13,10
hl_verify_end LABEL BYTE

hl_vol      db  'Displays the disk volume label and serial number, if they exist.',13,10
            db  13,10
            db  'VOL [drive:]',13,10
hl_vol_end  LABEL BYTE
