/**
 * \file      stringck.c
 * \author    Clemens Kresser
 * \date      Mar 25, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     Implements strnstr if it's not available
 *
 */

#include <string.h>
#include <stddef.h>

#ifndef strnstr
/**
 * \brief Searches for needle in haystack
 *
 * *haystack The string that should be scanned
 * *needle The wanted string
 * haystacklen Length of haystack
 *
 */
char *strnstr(char *haystack, char *needle, size_t haystacklen)
{
  size_t i;

  for (i = 0; i< haystacklen; i++)
  {
    if (strncmp(&haystack[i], needle, strlen(needle)) == 0)
      return &haystack[i];
  }

  return 0;
}
#endif
