/**
 * \file      log.c
 * \author    Clemens Kresser
 * \date      Apr 12, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     Logging for the ezwebsocket library
 *
 */

#include "log.h"

bool debugEnabled = false;

/**
 * \brief Enables debugging
 *
 * \param enable True => enable debugging, false => disable debugging
 */
void log_enableDebug (bool enable)
{
  debugEnabled = enable;
}
