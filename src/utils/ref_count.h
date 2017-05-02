/*
 * ref_count.h
 *
 *  Created on: Mar 26, 2017
 *      Author: clemens
 */

#ifndef UTILS_REF_COUNT_H_
#define UTILS_REF_COUNT_H_

void *refcnt_allocate(size_t size, void (*free)(void*));
void refcnt_ref(void *ptr);
void refcnt_unref(void *ptr);

#endif /* UTILS_REF_COUNT_H_ */
