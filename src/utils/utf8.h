/**
 * \file      utf8.h
 * \author    Clemens Kresser
 * \date      Apr 9, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     UTF8 validation functions that are used for the websockets
 *
 */

#ifndef UTILS_UTF8_H_
#define UTILS_UTF8_H_

#include <stdbool.h>
#include <stddef.h>

//! UTF8 validate states
enum utf8_state
{
  UTF8_STATE_OK,
  UTF8_STATE_FAIL,
  UTF8_STATE_BUSY,
};

enum utf8_state utf8_validate(char *string, size_t len, unsigned long *handle);

#endif /* UTILS_UTF8_H_ */
