/*
 * FCNTL.H - the flags open() takes
 */
#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_ACCMODE       0x0003
#define O_APPEND        0x0008  /* every write goes to the end */
#define O_CREAT         0x0100  /* create the file if it is not there */
#define O_TRUNC         0x0200  /* empty it if it is */
#define O_EXCL          0x0400  /* with O_CREAT: fail if it is there */
#define O_TEXT          0x4000  /* accepted; see IO.H */
#define O_BINARY        0x8000  /* accepted; see IO.H */

#endif
