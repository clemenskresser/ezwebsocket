/**
 * \file      ref_count.h
 * \author    Clemens Kresser
 * \date      Mar 26, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     Simple reference counting implementation
 *
 */

#ifndef UTILS_REF_COUNT_H_
#define UTILS_REF_COUNT_H_

void *refcnt_allocate(size_t size, void (*free)(void*));
void refcnt_ref(void *ptr);
void refcnt_unref(void *ptr);

#endif /* UTILS_REF_COUNT_H_ */
