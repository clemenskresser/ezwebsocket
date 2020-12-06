/**
 * \file      ref_count.c
 * \author    Clemens Kresser
 * \date      Mar 26, 2017
 * \copyright Copyright 2017-2020 Clemens Kresser. All rights reserved.
 * \license   This project is released under the MIT License.
 * \brief     simple reference counting implementation
 *
 */

#include <stdio.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>
#include "log.h"
#if (__STDC_VERSION__ >= 201112L)
#include <stdatomic.h>
#endif

struct ref_cnt_obj
{
  //! the number of reference holders
  volatile int cnt;
  //! the function that should be called to free the memory
  void (*pfnFree)(void*);
  //! mutex for the cnt variable
  pthread_mutex_t lock;
  //! the data itself
  unsigned char data[];
};

/**
 * \brief allocates a buffer with reference counting
 *
 * \param size the wanted size
 * \param void(*pfnFree)(void*) function that frees elements inside the buffer if necessary the buffer is freed on it's own
 *                            or NULL if not needed
 *
 * \return pointer to the allocated buffer;
 */
void *refcnt_allocate(size_t size, void (*pfnFree)(void*))
{
  struct ref_cnt_obj *ref;

  ref = malloc(sizeof(struct ref_cnt_obj) + size);
  if(!ref)
  {
    log_err("malloc failed");
    return NULL;
  }
  ref->cnt = 1;
  ref->pfnFree = pfnFree;
#if (__STDC_VERSION__ < 201112L)
  pthread_mutex_init(&ref->lock, NULL);
#endif

  return ref->data;
}

/**
 * \brief increments the reference count of the given object
 *
 * \param *ptr poiner to the object
 */
void refcnt_ref(void *ptr)
{
  struct ref_cnt_obj *ref;

  ref = (struct ref_cnt_obj*)(((unsigned char *)ptr) - offsetof(struct ref_cnt_obj, data));
#if (__STDC_VERSION__ < 201112L)
  pthread_mutex_lock(&ref->lock);
  ref->cnt++;
  pthread_mutex_unlock(&ref->lock);
#else
  atomic_fetch_add((int *)&ref->cnt, 1);
#endif
}

/**
 * \brief decrements the reference count of the given object and frees it if necessary
 *
 * \param *ptr poiner to the object
 */
void refcnt_unref(void *ptr)
{
  struct ref_cnt_obj *ref;

  ref = (struct ref_cnt_obj*)(((unsigned char*)ptr) - offsetof(struct ref_cnt_obj, data));
#if (__STDC_VERSION__ < 201112L)
  pthread_mutex_lock(&ref->lock);
  if (ref->cnt >= 0)
    ref->cnt--;
  else
    log_err("too many unrefs");
  pthread_mutex_unlock(&ref->lock);
#else
  atomic_fetch_sub((int *)&ref->cnt, 1);
  if (ref->cnt < 0)
    log_err("too many unrefs");
#endif

  if(ref->cnt == 0)
  {
#if (__STDC_VERSION__ < 201112L)
    pthread_mutex_destroy(&ref->lock);
#endif
    if(ref->pfnFree)
      ref->pfnFree(ref->data);
    free(ref);
  }
}
