/**
 * \file      dyn_buffer.c
 * \author    Clemens Kresser
 * \date      Mar 23, 2017
 * \copyright Copyright 2017-2020 Clemens Kresser. All rights reserved.
 * \license   This project is released under the MIT License.
 * \brief     dynamic buffers that are used to merge and split received data
 *
 */

#include "dyn_buffer.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "log.h"


/**
 * \brief: initializes a dynamic buffer
 *
 * \param *buffer: pointer to the buffer
 */
void dynBuffer_init(struct dyn_buffer *buffer)
{
  buffer->buffer = NULL;
  buffer->size = 0;
  buffer->used = 0;
}

/**
 * \brief: increases the buffer that it has the given amount of free memory
 *
 * \param *buffer: pointer to the buffer
 * \param numFreeBytes: the number of bytes that should be free after increase
 *
 * \return: 0 if successful else -1
 *
 */
int dynBuffer_increase_to(struct dyn_buffer *buffer, size_t numFreeBytes)
{
  if(buffer->buffer == NULL)
  {
    buffer->buffer = malloc(numFreeBytes);
    if(!buffer->buffer)
    {
      log_err("malloc failed");
      return -1;
    }

    buffer->size = numFreeBytes;
    buffer->used = 0;
  }
  else
  {
    char *newbuf;

    if(buffer->size - buffer->used < numFreeBytes)
    {
      buffer->size = buffer->used + numFreeBytes;
      newbuf = realloc(buffer->buffer, buffer->size);
      if(!newbuf)
      {
        free(buffer->buffer);
        buffer->buffer = NULL;
        buffer->size = 0;
        buffer->used = 0;
        log_err("realloc failed");
        return -1;
      }

      buffer->buffer = newbuf;
    }
  }
  return 0;
}

/**
 * \brief: removes the given amount of leading bytes from the buffer
 *
 * \param *buffer: pointer to the buffer
 * \param count: the number of bytes that should be removed
 *
 * \return 0 if successful else -1
 */
int dynBuffer_removeLeadingBytes(struct dyn_buffer *buffer, size_t count)
{
  if(buffer->buffer == NULL)
  {
    log_err("empty buffer");
    return -1;
  }

  if(!count)
    return 0;

  if(buffer->used < count)
  {
    log_err("not enough bytes in buffer");
    return -1;
  }

  if(buffer->used > count)
  {
    buffer->used = buffer->used - count;
    memmove(&buffer->buffer[0], &buffer->buffer[count], buffer->used);
  }
  else
  {
    free(buffer->buffer);
    buffer->buffer = NULL;
    buffer->used = 0;
    buffer->size = 0;
  }

  return 0;
}

/**
 * \brief: deallocates all memory that was allocated by the buffer
 *
 * \param *buffer: pointer to the buffer
 *
 * \return: 0 if successful else -1
 *
 */
int dynBuffer_delete(struct dyn_buffer *buffer)
{
  if (!buffer->buffer)
    return -1;

  free(buffer->buffer);
  buffer->buffer = NULL;
  buffer->size = 0;
  buffer->used = 0;

  return 0;
}

