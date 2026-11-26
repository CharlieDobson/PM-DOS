/*
 * XML.H - XML read a piece at a time from a whole document in memory,
 * and the little that writing it takes.
 *
 * READING.  xml_next hands back the document's pieces in order: an
 * element's start (with its attributes), its end, text.  Comments,
 * processing instructions and the DOCTYPE are passed over.  An element
 * written <a/> is a start and then an end, so every start has its end.
 *
 * NAMESPACES are what the two formats that use this are made of, and a
 * file may call them by any prefix it likes; so a reader says first
 * which namespaces it knows (xml_ns: a URI and a small number of its
 * own choosing), and every element and attribute is then given as that
 * number and the name after the colon.  0 is "none, or one nobody
 * asked about".  Several URIs may have one number - that is how the
 * "strict" and "transitional" spellings of a format are read alike.
 *
 * TEXT AND VALUES are UTF-8 with the entities already turned into the
 * characters they stand for.  An attribute's value ends with a 0 byte;
 * text does not, and has a length.  Names are pieces of the document
 * itself, with a length.
 *
 * A document that is not well formed - an end that matches no start,
 * elements more than XML_MAX_DEPTH deep, a tag that never closes -
 * gives XML_ERROR, and goes on giving it.
 */
#ifndef XML_H
#define XML_H

#include "fmt.h"

enum { XML_EOF, XML_START, XML_END, XML_TEXT, XML_ERROR };

#define XML_MAX_DEPTH   256

/* the namespace xml:space and xml:id are in, for xml_ns */
#define XML_NS_URI      "http://www.w3.org/XML/1998/namespace"

typedef struct {
    int          ns;            /* the namespace's number */
    const char  *name;          /* the name after the colon: name_len bytes */
    unsigned int name_len;
    const char  *value;         /* ends with a 0 */
    unsigned int value_len;
    unsigned int at;            /* xml_next's own */
} XATTR;

typedef struct XML XML;

struct XML {
    /* the piece xml_next last gave */
    int          token;         /* XML_*: which kind it was */
    int          ns;            /* XML_START, XML_END: the element's namespace */
    const char  *name;          /* ...and its name: name_len bytes */
    unsigned int name_len;
    int          depth;         /* ...and how deep it is: 1 for the outermost */
    XATTR       *attr;          /* XML_START: its attributes */
    unsigned int nattr;
    const char  *text;          /* XML_TEXT: text_len bytes */
    unsigned int text_len;
};

/* what xml_mark keeps: a place to come back to */
typedef struct {
    unsigned int pos;
    unsigned int level, nbind;
    int          pending;
} XMARK;

/*
 * A document in "len" bytes at "data", which stay where they are until
 * xml_close.  UTF-8, with or without a byte-order mark, or UTF-16 with
 * one.  NULL if there is no memory.
 */
XML  *xml_open( const uint8_t *data, uint32_t len );
void  xml_close( XML *xml );
/* a namespace the reader knows, and the number (not 0) it is given as.
   "uri" must stay where it is.  0 if there is no memory. */
int   xml_ns( XML *xml, const char *uri, int id );
int   xml_next( XML *xml );

/* the element just given: is it this one? */
int   xml_is( const XML *xml, int ns, const char *name );
/* an attribute of the start just given: its value, or NULL */
const char *xml_attr( const XML *xml, int ns, const char *name );
/* after a start: everything up to and including its end is passed
   over.  0 if the document ends or goes wrong first. */
int   xml_skip( XML *xml );
/*
 * An element's children one at a time: after the start of the element
 * at "depth" (its xml->depth), each call gives 1 at the start of its
 * next child and 0 when the element has ended - or the document has,
 * or has gone wrong.  A child the caller looked at and did not read
 * through is passed over, so a caller reads only what it wants:
 *
 *     depth = xml->depth;
 *     while ( xml_child( xml, depth ) ) {
 *         if ( xml_is( xml, NS, "wanted" ) ) { ... }
 *     }
 */
int   xml_child( XML *xml, int depth );
/* after a start: the text inside the element, whatever elements it is
   in, gathered into "buf" (cut to fit, ended with a 0), up to and
   including the element's end.  0 as for xml_skip. */
int   xml_gather( XML *xml, char *buf, unsigned int max );
/* a place kept and gone back to, to read something twice: marked
   after an element's start, and gone back to before anything after
   that element's end has been read */
void  xml_mark( const XML *xml, XMARK *mark );
void  xml_rewind( XML *xml, const XMARK *mark );

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Text put into a document being written, with what XML reserves
 * written as entities and what it forbids left out (the control
 * characters other than tab, line feed and return).  "quote" not 0 is
 * for an attribute's value: quotation marks are written as entities
 * too, and a tab or a line end as a character reference, so that it
 * comes back as it went in.
 */
void  xml_esc( OUT *out, const char *text, int quote );                      /* UTF-8, to a 0 */
void  xml_esc_n( OUT *out, const uint8_t *text, unsigned int len, int quote );   /* UTF-8 */
void  xml_esc16( OUT *out, const uint16_t *text, unsigned int len, int quote );  /* a document's own */

#endif
