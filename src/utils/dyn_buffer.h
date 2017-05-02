/*
 * dyn_buffer.h
 *
 *  Created on: Mar 23, 2017
 *      Author: Clemens Kresser
 *      License: MIT
 */

#ifndef DYN_BUFFER_H_
#define DYN_BUFFER_H_


#include <stddef.h>

struct dyn_buffer
{
  char *buffer;
  int used;
  int size;
};

#define DYNBUFFER_INCREASE_STEPS 1024

#define DYNBUFFER_WRITE_POS(buf) (&((buf)->buffer[(buf)->used]))
#define DYNBUFFER_INCREASE_WRITE_POS(buf, bytes) ((buf)->used += bytes)
#define DYNBUFFER_BYTES_FREE(buf) ((buf)->size - (buf)->used)
#define DYNBUFFER_SIZE(buf) ((buf)->used)
#define DYNBUFFER_BUFFER(buf) ((buf)->buffer)

void dynBuffer_init(struct dyn_buffer *buffer);
int dynBuffer_increase_to(struct dyn_buffer *buffer, size_t numFreeBytes);
int dynBuffer_removeTrailingBytes(struct dyn_buffer *buffer, size_t count);
int dynBuffer_delete(struct dyn_buffer *buffer);

#endif /* DYN_BUFFER_H_ */
