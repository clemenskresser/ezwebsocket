/**
 * \file      log.h
 * \author    Clemens Kresser
 * \date      Apr 12, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     Logging for the ezwebsocket library
 *
 */

#ifndef UTILS_LOG_H_
#define UTILS_LOG_H_

#include <stdbool.h>

void log_enableDebug (bool enable);
extern bool debugEnabled;

//! MACRO for printing errors
#define log_err(FMT, ...)      do { fprintf(stderr, "%s:%s: " FMT "\n", __FILE__, __func__, ##__VA_ARGS__); } while (0)
//! MACRO for printing debug messages
#define log_dbg(FMT, ...)      do { if (debugEnabled) {fprintf(stdout, "%s:%s: " FMT "\n", __FILE__, __func__, ##__VA_ARGS__);} } while (0)


#endif /* UTILS_LOG_H_ */
