#ifndef LINEBUF_H
#define LINEBUF_H

#include <stddef.h>

struct linebuf;

struct linebuf*			 linebuf_create(void);
void				 linebuf_free(struct linebuf *);

char				*linebuf_read(struct linebuf *);
int				 linebuf_fill(struct linebuf *,
				     const char *, size_t sz);

#endif
