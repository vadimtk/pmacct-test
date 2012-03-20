/*
 * Packet interface
 * Copyright (C) 1999 Kunihiro Ishiguro
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.  
 */

#ifndef _STREAM_H_
#define _STREAM_H_

#include "prefix.h"

/*
 * A stream is an arbitrary buffer, whose contents generally are assumed to
 * be in network order.
 *
 * A stream has the following attributes associated with it:
 *
 * - size: the allocated, invariant size of the buffer.
 *
 * - getp: the get position marker, denoting the offset in the stream where
 *         the next read (or 'get') will be from. This getp marker is
 *         automatically adjusted when data is read from the stream, the
 *         user may also manipulate this offset as they wish, within limits
 *         (see below)
 *
 * - endp: the end position marker, denoting the offset in the stream where
 *         valid data ends, and if the user attempted to write (or
 *         'put') data where that data would be written (or 'put') to.
 *
 * These attributes are all size_t values.
 *
 * Constraints:
 *
 * 1. getp can never exceed endp
 *
 * - hence if getp is equal to endp, there is no more valid data that can be
 *   gotten from the stream (though, the user may reposition getp to earlier in
 *   the stream, if they wish).
 *
 * 2. endp can never exceed size
 *
 * - hence, if endp is equal to size, then the stream is full, and no more
 *   data can be written to the stream.
 *
 * In other words the following must always be true, and the stream
 * abstraction is allowed internally to assert that the following property
 * holds true for a stream, as and when it wishes:
 *
 *        getp <= endp <= size
 *
 * It is the users responsibility to ensure this property is never violated.
 *
 * A stream therefore can be thought of like this:
 *
 * 	---------------------------------------------------
 * 	|XXXXXXXXXXXXXXXXXXXXXXXX                         |
 * 	---------------------------------------------------
 *               ^               ^                        ^
 *               getp            endp                     size
 *
 * This shows a stream containing data (shown as 'X') up to the endp offset.
 * The stream is empty from endp to size. Without adjusting getp, there are
 * still endp-getp bytes of valid data to be read from the stream.
 *
 * Methods are provided to get and put to/from the stream, as well as 
 * retrieve the values of the 3 markers and manipulate the getp marker.
 *
 * Note:
 * At the moment, newly allocated streams are zero filled. Hence, one can
 * use stream_forward_endp() to effectively create arbitrary zero-fill
 * padding. However, note that stream_reset() does *not* zero-out the
 * stream. This property should **not** be relied upon.
 *
 * Best practice is to use stream_put (<stream *>, NULL, <size>) to zero out
 * any part of a stream which isn't otherwise written to.
 */

/* Stream buffer. */
struct stream
{
  size_t getp; 		/* next get position */
  size_t endp;		/* last valid data position */
  size_t size;		/* size of data segment */
  unsigned char *data; /* data pointer */
};

/* Utility macros. */
#define STREAM_SIZE(S)  ((S)->size)
  /* number of bytes which can still be written */
#define STREAM_WRITEABLE(S) ((S)->size - (S)->endp)
  /* number of bytes still to be read */
#define STREAM_READABLE(S) ((S)->endp - (S)->getp)

/* deprecated macros - do not use in new code */
#define STREAM_PNT(S)   stream_pnt((S))
#define STREAM_DATA(S)  ((S)->data)
#define STREAM_REMAIN(S) STREAM_WRITEABLE((S))

/* Stream prototypes. 
 * For stream_{put,get}S, the S suffix mean:
 *
 * c: character (unsigned byte)
 * w: word (two bytes)
 * l: long (two words)
 * q: quad (four words)
 */
#if (!defined __STREAM_C)
#define EXT extern
#else
#define EXT
#endif
EXT struct stream *stream_new (size_t);
EXT void stream_free (struct stream *);
EXT struct stream * stream_copy (struct stream *, struct stream *src);
EXT struct stream *stream_dup (struct stream *);
EXT size_t stream_resize (struct stream *, size_t);
EXT size_t stream_get_getp (struct stream *);
EXT size_t stream_get_endp (struct stream *);
EXT size_t stream_get_size (struct stream *);
EXT u_char *stream_get_data (struct stream *);
EXT void stream_set_getp (struct stream *, size_t);
EXT void stream_forward_getp (struct stream *, size_t);
EXT void stream_forward_endp (struct stream *, size_t);
EXT void stream_put (struct stream *, const void *, size_t);
EXT int stream_putc (struct stream *, u_char);
EXT int stream_putc_at (struct stream *, size_t, u_char);
EXT int stream_putw (struct stream *, u_int16_t);
EXT int stream_putw_at (struct stream *, size_t, u_int16_t);
EXT int stream_putl (struct stream *, u_int32_t);
EXT int stream_putl_at (struct stream *, size_t, u_int32_t);
EXT int stream_putq (struct stream *, uint64_t);
EXT int stream_putq_at (struct stream *, size_t, uint64_t);
EXT int stream_put_ipv4 (struct stream *, u_int32_t);
EXT int stream_put_in_addr (struct stream *, struct in_addr *);
EXT int stream_put_prefix (struct stream *, struct isis_prefix *);
EXT void stream_get (void *, struct stream *, size_t);
EXT u_char stream_getc (struct stream *);
EXT u_char stream_getc_from (struct stream *, size_t);
EXT u_int16_t stream_getw (struct stream *);
EXT u_int16_t stream_getw_from (struct stream *, size_t);
EXT u_int32_t stream_getl (struct stream *);
EXT u_int32_t stream_getl_from (struct stream *, size_t);
EXT uint64_t stream_getq (struct stream *);
EXT uint64_t stream_getq_from (struct stream *, size_t);
EXT u_int32_t stream_get_ipv4 (struct stream *);

#undef stream_read
#undef stream_write

/* Deprecated: assumes blocking I/O.  Will be removed. 
   Use stream_read_try instead.  */
EXT int stream_read (struct stream *, int, size_t);

/* Deprecated: all file descriptors should already be non-blocking.
   Will be removed.  Use stream_read_try instead. */
EXT int stream_read_unblock (struct stream *, int, size_t);

/* Read up to size bytes into the stream.
   Return code:
     >0: number of bytes read
     0: end-of-file
     -1: fatal error
     -2: transient error, should retry later (i.e. EAGAIN or EINTR)
   This is suitable for use with non-blocking file descriptors.
 */
EXT ssize_t stream_read_try(struct stream *, int, size_t);
EXT ssize_t stream_recvmsg (struct stream *, int, struct msghdr *, int, size_t);
EXT ssize_t stream_recvfrom (struct stream *, int, size_t, int, struct sockaddr *, socklen_t *);
EXT size_t stream_write (struct stream *, const void *, size_t);
EXT void stream_reset (struct stream *);
EXT int stream_flush (struct stream *, int);
EXT int stream_empty (struct stream *); /* is the stream empty? */
EXT u_char *stream_pnt (struct stream *);
EXT int readn (int, u_char *, int);
#undef EXT

#endif /* _STREAM_H_ */