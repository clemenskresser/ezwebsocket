/**
 * \file      dyn_buffer.h
 * \author    Clemens Kresser
 * \date      Mar 23, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     Dynamic buffers that are used to merge and split received data
 *
 */

#ifndef DYN_BUFFER_H_
#define DYN_BUFFER_H_


#include <stddef.h>

//! structure that contains all necessary information of the dynamic buffer
struct dyn_buffer
{
  //! pointer to the buffer that holds the data
  char *buffer;
  //! the number of used bytes
  size_t used;
  //! the size of the buffer
  size_t size;
};

//! The minimum allocated amount of bytes
#define DYNBUFFER_INCREASE_STEPS 1024

//! returns the write position of the dynbuffer
#define DYNBUFFER_WRITE_POS(buf) (&((buf)->buffer[(buf)->used]))
//! increases the write position of the dynbuffer
#define DYNBUFFER_INCREASE_WRITE_POS(buf, bytes) ((buf)->used += bytes)
//! returns the remaining space of the dynbuffer
#define DYNBUFFER_BYTES_FREE(buf) ((buf)->size - (buf)->used)
//! returns the current size of the dynbuffer
#define DYNBUFFER_SIZE(buf) ((buf)->used)
//! returns the base address of the buffer
#define DYNBUFFER_BUFFER(buf) ((buf)->buffer)

void dynBuffer_init(struct dyn_buffer *buffer);
int dynBuffer_increase_to(struct dyn_buffer *buffer, size_t numFreeBytes);
int dynBuffer_removeLeadingBytes(struct dyn_buffer *buffer, size_t count);
int dynBuffer_delete(struct dyn_buffer *buffer);

#endif /* DYN_BUFFER_H_ */
