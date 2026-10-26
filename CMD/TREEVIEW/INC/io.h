/*
 * IO.H - XARC's C library: the one handle call the backend makes.
 * _chsize cuts a file (or lengthens it) to "size" bytes: a seek there
 * and a write of nothing, as DOS does it.
 */
#ifndef _XA_IO_H
#define _XA_IO_H

int _chsize( int handle, long size );
#define chsize  _chsize

#endif
