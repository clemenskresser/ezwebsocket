/**
 * \file      stringck.c
 * \author    Clemens Kresser
 * \date      Mar 25, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     Implements strnstr if it's not available
 *
 */

#ifndef UTILS_STRINGCK_H_
#define UTILS_STRINGCK_H_

#ifndef strnstr
char *strnstr(char *haystack, char *needle, size_t haystacklen);
#endif

#endif /* UTILS_STRINGCK_H_ */
