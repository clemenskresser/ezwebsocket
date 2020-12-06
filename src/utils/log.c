/**
 * \file      log.c
 * \author    Clemens Kresser
 * \date      Apr 12, 2017
 * \copyright Copyright 2017-2020 Clemens Kresser. All rights reserved.
 * \license   This project is released under the MIT License.
 * \brief     logging for the ezwebsocket library
 *
 */

#include "log.h"

bool debugEnabled = false;

/**
 * \brief enables debugging
 *
 * \param enable true => enable debugging, false => disable debugging
 */
void log_enableDebug (bool enable)
{
  debugEnabled = enable;
}
