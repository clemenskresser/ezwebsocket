/**
 * \file      utf8.c
 * \author    Clemens Kresser
 * \date      Apr 9, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     UTF8 validation functions that are used for the websockets
 *
 */

#include "utf8.h"
#include <stdio.h>

/**
 * \brief Checks a single character if it is valid utf8
 *
 * \param c The character that should be checked
 * \param[in,out] *handle Stores the last characters if needed start with 0
 *
 * \return UTF8_STATE_OK, UTF8_STATE_FAIL or UTF8_STATE_BUSY if more characters are needed
 */
enum utf8_state utf8_validate_single(char c, unsigned long *handle)
{
  unsigned long data = (unsigned char) c;

  if(!*handle)
  {
    if(data <= 0x7F)
      return UTF8_STATE_OK;
    else if((data & 0xE0) == 0xC0)
    {
      *handle = (data & 0x1F) << (1 * 6);
      *handle |= 0x50000000;
    }
    else if((data & 0xF0) == 0xE0)
    {
      *handle = (data & 0x0F) << (2 * 6);
      *handle |= 0xa0000000;
    }
    else if((data & 0xF8) == 0xF0)
    {
      *handle = (data & 0x07) << (3 * 6);
      *handle |= 0xf0000000;
    }
    else
      return UTF8_STATE_FAIL;

    if((*handle & 0x0FFFFFFF) > 0x10FFFF)
      return UTF8_STATE_FAIL;
    else
      return UTF8_STATE_BUSY;
  }
  else
  {
    if((data & 0xC0) != 0x80)
      return UTF8_STATE_FAIL;

    *handle -= 0x40000000;
    *handle |= ((data & 0x3F) << (6 * (*handle >> 30)));

    if((*handle & 0x0FFFFFFF) > 0x10FFFF)
      return UTF8_STATE_FAIL;

    if(*handle & 0xC0000000)
      return UTF8_STATE_BUSY;
    else
    {
      switch(*handle & 0x30000000)
      {
        case 0x30000000:
          if((*handle & 0x0FFFFFFF) < 65536)
            return UTF8_STATE_FAIL;
          break;
        case 0x20000000:
          if((*handle & 0x0FFFFFFF) < 2048)
            return UTF8_STATE_FAIL;
          break;
        case 0x10000000:
          if((*handle & 0x0FFFFFFF) < 128)
            return UTF8_STATE_FAIL;
          break;
      }

      if(((*handle & 0x0FFFFFFF) >= 0xD800) && ((*handle & 0x0FFFFFFF) <= 0xDFFF))
        return UTF8_STATE_FAIL;

      *handle = 0;
      return UTF8_STATE_OK;
    }
  }
}

/**
 * \brief Validates if the given string is valid UTF8
 *
 * \param *string Pointer to the string that should be checked
 * \param len The length of the string
 * \param *handle Pointer to the handle that is used to store the state if using fragmented strings start with 0
 *
 *  UTF8_STATE_OK, UTF8_STATE_FAIL or UTF8_STATE_BUSY if more characters are needed
 */
enum utf8_state utf8_validate(char *string, size_t len, unsigned long *handle)
{
  enum utf8_state state = UTF8_STATE_OK;

  while(len--)
  {
    if((state = utf8_validate_single(*string, handle)) == UTF8_STATE_FAIL)
    {
      return state;
    }
    string++;
  }
  return state;
}
