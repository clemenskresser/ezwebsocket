/**
 * \file      base64.c
 * \author    Clemens Kresser
 * \date      Apr 14, 2017
 * \copyright Copyright  2017-2021 Clemens Kresser. All rights reserved.
 * \license   This project is released under the MIT License.
 * \brief     Handles base64 encoding
 *
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"

static const char base64_table[64] =
{
  'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
  'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
  'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
  'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
  'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
  'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
  'w', 'x', 'y', 'z', '0', '1', '2', '3',
  '4', '5', '6', '7', '8', '9', '+', '/'
};

/**
 * \brief encodes the given 3 bytes to wrBuffer
 *
 * \param byte0 1st byte to encode
 * \param byte1 2nd byte to encode
 * \param byte2 3rd byte to encode
 * \param [out] wrBuffer Pointer to where the data is written (size must be at least 4 bytes)
 *
 */
static inline void encode(unsigned char byte0, unsigned char byte1, unsigned char byte2, char *wrBuffer)
{
  wrBuffer[0] = base64_table[(byte0 & 0xFC) >> 2];
  wrBuffer[1] = base64_table[((byte0 & 0x03) << 4) | ((byte1 & 0xF0) >> 4)];
  wrBuffer[2] = base64_table[((byte1 & 0x0F) << 2) | ((byte2 & 0xC0) >> 6)];
  wrBuffer[3] = base64_table[(byte2 & 0x3F)];
}

/**
 * \brief encodes the given data to base64 format
 *
 * \param *data Pointer to the data that should be encoded
 * \param len The length of the data
 *
 * \return base64 encoded string or NULL
 *
 *  WARNING: return value must be freed after use!
 *
 */
char *base64_encode(unsigned char *data, size_t len)
{
  char *encString = malloc(((len + 2) / 3) * 4 + 1);
  char *ptr;
  size_t i;
  unsigned char help[3];
  int count;

  if(!encString)
  {
    log_err("malloc failed");
    return NULL;
  }
  ptr = encString;

  for(i = 0; i < (len / 3) * 3; i += 3)
  {
    encode(data[i + 0], data[i + 1], data[i + 2], ptr);
    ptr += 4;
  }

  //check if we need to pad with 0
  if(i < len)
  {
    help[0] = 0;
    help[1] = 0;
    help[2] = 0;

    count = 0;

    for(; i < len; i++)
    {
      help[count] = data[i];
      count++;
    }

    encode(help[0], help[1], help[2], ptr);
    ptr += (count + 1);

    //pad with '='
    for(; count < 3; count++)
    {
      *ptr = '=';
      ptr++;
    }
  }
  *ptr = '\0';
  return encString;
}
