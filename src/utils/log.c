/*
 * log.c
 *
 *  Created on: Apr 12, 2017
 *      Author: Clemens Kresser
 *      License: MIT
 */


#include "log.h"

bool debugEnabled = false;

void log_enableDebug (bool enabled)
{
  debugEnabled = enabled;
}
