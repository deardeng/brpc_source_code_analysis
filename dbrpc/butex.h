//
// Created by dengxin05 on 2020/6/25.
//

#ifndef BTHREAD_BUTEX_H
#define BTHREAD_BUTEX_H

#include <errno.h>              // users need to check errno
#include <time.h>               // timespec
#include "butil/macros.h"       // BAIDU_CASSERT
#include "bthread/types.h"      // bthread_t

namespace bthread{
// Create a butex which is a futex-like 32-bit primitive for synchronizing
// bthreads/pthreads.
// Returns a pointer to 32-bit data, NULL on failure.
// Note: all butexes are private(not inter-process)
void* butex_create();

// Check width of user type before casting.
template <typename T> T* butex_create_checked(){
  BAIDU_CASSERT(sizeof(T) == sizeof(int), sizeof_T_must_equal_int);
  return static_cast<T*>(butex_create());
}

// Destroy the butex.
void butex_destroy(void* butex);

// Wake up at most 1 thread waiting on |butex|.
// Returns # of threads woken up.
int butex_wake(void* butex);

// Wake up all threads waiting on |butex|.
// Returns # of threads woken up.
int butex_wake_all(void* butex);

// Wake up all threads waiting on |butex| except a bthread whose identifier
// is |excluded_bthread|. This function does not yield.
int butex_wake_except(void* butex, bthread_t exclude_bthread);

// Wake up at most 1 thread waiting on |butex1|, let all other thread wait
// on |butex2| instead.
// Returns # of threads woken up.
int butex_requeue(void* butex1, void* butex2);

// Atomically wait on |butex| if *butex equals |expected_value|, until the
// butex is woken up butex_wake*, or CLOCK_REALTIME reached |abstime| if
// abstime is not NULL.
//    Different from FUTEX_WAIT, butex_wait uses absolute time.
// Returns 0 on success, -1 otherwise and errno is set.
int butex_wait(void* butex, int expected_value, const timespec* abstime);

} // namespace bthread

#endif //BTHREAD_BUTEX_H
