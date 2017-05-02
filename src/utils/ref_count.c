/*
 * ref_count.c
 *
 *  Created on: Mar 26, 2017
 *      Author: clemens
 */

#include <stdio.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <pthread.h>
#include "log.h"

struct ref_cnt_obj
{
  int cnt;
  void (*pfnFree)(void*);
  pthread_mutex_t lock;
  unsigned char data[];
};

/**
 * \brief: allocates a buffer with reference counting
 *
 * \param size: the wanted size
 * \param void(*pfnFree)(void*): function that frees elements inside the buffer if necessary the buffer is freed on it's own
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
  pthread_mutex_init(&ref->lock, NULL); //todo: use     atomic_fetch_add((int *)&ref->count, 1); in case of C11

  return ref->data;
}

/**
 * \brief: increments the reference count of the given object
 *
 * \param *ptr: poiner to the object
 */
void refcnt_ref(void *ptr)
{
  struct ref_cnt_obj *ref;

  ref = ptr - offsetof(struct ref_cnt_obj, data);
  pthread_mutex_lock(&ref->lock);    //todo: use     atomic_fetch_add((int *)&ref->count, 1); in case of C11
  ref->cnt++;
  pthread_mutex_unlock(&ref->lock);
}

/**
 * \brief: decrements the reference count of the given object and frees it if necessary
 *
 * \param *ptr: poiner to the object
 */
void refcnt_unref(void *ptr)
{
  struct ref_cnt_obj *ref;

  ref = ptr - offsetof(struct ref_cnt_obj, data);
  pthread_mutex_lock(&ref->lock);    //todo: use     atomic_fetch_add((int *)&ref->count, 1); in case of C11
  if (ref->cnt >= 0)
    ref->cnt--;
  else
    log_err("to many unrefs");
  pthread_mutex_unlock(&ref->lock);

  if(ref->cnt == 0)
  {
    pthread_mutex_destroy(&ref->lock);
    if(ref->pfnFree)
      ref->pfnFree(ref->data);
    free(ref);
  }
}
