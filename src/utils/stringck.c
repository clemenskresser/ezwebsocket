/*
 * stringck.c
 *
 *  Created on: Mar 25, 2017
 *      Author: Clemens Kresser
 *      License: MIT
 */

#include <string.h>
#include <stddef.h>

/**
 * \brief: searches for needle in haystack
 *
 * *haystack: the string that should be scanned
 * *needle: the wanted string
 * haystacklen: length of haystack
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
