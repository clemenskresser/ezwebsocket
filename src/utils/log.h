/*
 * log.h
 *
 *  Created on: Apr 12, 2017
 *      Author: Clemens Kresser
 *      License: MIT
 */

#ifndef UTILS_LOG_H_
#define UTILS_LOG_H_

#include <stdbool.h>

void log_enableDebug (bool enabled);
extern bool debugEnabled;

#define log_err(FMT, ...)      do { fprintf(stderr, "%s:%s: " FMT "\n", __FILE__, __func__, ##__VA_ARGS__); } while (0)
#define log_dbg(FMT, ...)      do { if (debugEnabled) {fprintf(stdout, "%s:%s: " FMT "\n", __FILE__, __func__, ##__VA_ARGS__);} } while (0)


#endif /* UTILS_LOG_H_ */
