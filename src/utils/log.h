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

#define log_err(FMT, ARGS...)      do { fprintf(stderr, "%s:%s: " FMT "\n", __FILE__, __FUNCTION__, ##ARGS); } while (0)
#define log_dbg(FMT, ARGS...)      do { if (debugEnabled) {fprintf(stdout, "%s:%s: " FMT "\n", __FILE__, __FUNCTION__, ##ARGS);} } while (0)


#endif /* UTILS_LOG_H_ */
