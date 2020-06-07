//
// Created by dengxin on 2020/6/7.
//

#ifndef BTHREAD_BTHREAD_H
#define BTHREAD_BTHREAD_H

#include <pthread.h>
#include <sys/socket.h>
#include "bthread/types.h"
#include "bthread/errno.h"

#if defined(__cplusplus)
#include <iostream>
#include "bthread/mutex.h"
#endif

#include "bthread/id.h"

__BEGIN_DECLS

// Create bthread `fn(args) with attributes `attr' and put the identifier into
// `tid'. Switch to the new thread and schedule old thread to run. Use this
// function when the new thread is more urgent.
// Returns 0 on success, errno otherwise.
extern int bthread_start_urgent(bthread_t* __restrict tid,
        const bthread_attr_t* __restrict attr,
        void * (*fn)(void*),
        void * __restrict args);

// Create bthread `fn(args)' with attributes `attr' and put the identifier into
// `tid'. This function behaves closer to pthread_create: after scheduling the new
// thread to run, it returns. In another word, the new thread may take longer time
// than bthread_start_urgent() to run.
// Return 0 on success, errno otherwise.
extern int bthread_start_background(bthread_t* __restrict tid,
        const bthread_attr_t* __restrict attr,
        void *(*fn)(void*),
        void * __restrict args);

// Wake up operations blocking the thread. Different functions may behave
// differently:
//   bthread_usleep(): return -1 and sets errno to ESTOP if bthread_stop()
//                     is called, or to EINTR otherwise.
//   bthread_wait(): returns -1 and sets errno to EINTR
//   bthread_mutex_*lock: unaffected(still blocking)
//   bthread_cond_*wait: wakes up and returns 0.
//   bthread_*join: unaffected.
// Common usage of interruption is to make a thread to quit ASAP.
//      [Thread1]                   [Thread2]
//      set stopping flag
//      bthread_interrupt(Thread2)
//                                  wake up
//                                  see the flag and quit
//                                  may block again if the flag is unchanged
// bthread_interrupt() guarantees that Tthread2 is woken up reliably no matter
// how the 2 threads are interleaved.
// Return 0 on success, errno otherwise.
extern int bthread_interrupt(bthread_t tid);

// Make bthread_stopped() on the bthread return true and interrupt the bthread.
// Note that current bthread_stop() solely sets the built-in "stop flag" and
// calls bthread_interrupt(), which is different from earlier version of bthread,
// and replaceable by user-define stop flags plus calls to bthread_interrupt().
// Returns 0 on success, errno otherwise.
extern int bthread_stop(bthread_t tid);

// Returns 1 iff bthread_stop(tid) was called or the thread does not exist,
// 0 otherwise.
extern int bthread_stopped(bthread_t tid);

// Returns identifier of caller if caller is a bthread, 0 otherwise(Id of a
// bthread is never zero)
extern bthread_t bthread_self(void);

// Compare two bthread identifiers.
// Returns a non-zero value if t1 and t2 are equal, zero otherwise.
extern int bthread_equal(bthread_t t1, bthread_t t2);

// Terminate calling bthread/pthread and make `retval' available to any
// successful join with the terminating thread. This function does not return.
extern void bthread_exit(void* retval) __attribute__((__noreturn__));

// Make calling thread wait for termination of bthread `bt'. Return immediately
// if `bt' is already terminated.
// Notes:
// - All bthreads are "detached" but still joinable.
// - *bthread_return is always set to null. If you need to return value
// from a bthread, pass the value via the `args' created the bthread.
// - bthread_join() is not affected by bthread_interrupt.
// Returns 0 on success, errno otherwise.
extern int bthread_join(bthread_t bt, void** bthread_return);

// Track and join many bthreads.
// Notice that all bthread_list* functions are NOT thread-safe.
extern int bthread_list_init(bthread_list_t* list, unsigned size, unsigned conflict_size);
extern void bthread_list_add(bthread_list_t* list, bthread_t tid);
extern int bthread_list_stop(bthread_list_t* list);
extern int bthread_list_join(bthread_list_t* list);

// -------------------------------------
// Functions for handling attributes.
// -------------------------------------

// Initialize thread attribute `attr' with default attributes.
extern int bthread_attr_init(bthread_attr_t* attr);

// Destory thread attribute `attr'.
extern int bthread_attr_destroy(bthread_attr_t* attr);

// Initialize bthread attribute `attr' with attributes corresponding to the
// already running bthread `bt'. It shall be called on uninitialized `attr'
// and destroyed with bthread_attr_destroy when no longer needed.
extern int bthread_getattr(bthread_t bt, bthread_attr_t* attr);

// --------------------------------------
// Functions for scheduling control.
// --------------------------------------

// Get number of worker pthreads
extern int bthread_getconcurrency(void);

// Set number of worker pthreads to `num'. After a successful call,
// bthread_getconcurrency() shall return new set number, but workers may
// take some time to quit or create.
// Note: currently concurrency cannot be reduced after any bthread created.
extern int bthread_setconcurrency(int num);

// Yield processor to another bthread.
// Notice that current implementation is not fair, which means that
// even if bthread_yield() is called, suspended threads may still starve.
extern int bthread_yield(void);

// Suspend current thread for at least `microseconds'
// Interruptible by thread_interrupt().
extern int bthread_usleep(uint64_t microseconds);

// ---------------------------------------
// Functions for mutex handling.
// ---------------------------------------



#endif //BTHREAD_BTHREAD_H
