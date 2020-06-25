//
// Created by dengxin05 on 2020/6/25.
//

#include "butil/atomicops.h"                    // butil::atomic
#include "butil/scoped_lock.h"                  // BAIDU_SCOPED_LOCK
#include "butil/macros.h"
#include "butil/containers/liked_list.h"        // LinkNode
#ifdef SHOW_BTHREAD_BUTEX_WAITER_COUNT_IN_VARS
#include "butil/memory/singleton_on_pthread_once.h"
#endif
#include "butil/logging.h"
#include "butil/object_pool.h"
#include "bthread/errno.h"                      // EWOULDBLOCK
#include "bthread/sys_futex.h"                  // futex_*
#include "bthread/processor.h"                  // cpu_relax
#include "bthread/task_control.h"               // TaskControl
#include "bthread/task_group.h"                 // TaskGroup
#include "bthread/timer_thread.h"
#include "bthread/butex.h"
#include "bthread/mutex.h"

// This file implements butex.h
// Provides futex-like semantics which is sequenced wait and wake operations
// and guaranteed visibilities
//
// If wait is sequenced before wake:
//    [thread1]               [thread2]
//     wait()                  value = new_value
//                             wake()
// wait() sees unmatched value(fail to wait), or wake() sees the waiter.
//
// If wait is sequenced after wake:
//     [thread1]              [thread2]
//                            value = new_value
//                            wake()
//      wait()
// wake() must provide some sort of memory fence to prevent assignment
// of value to be reordered after it. thus the value is visible to wait()
// as well.

namespace bthread{

#ifndef SHOW_BTHREAD_BUTEX_WAITER_COUNT_IN_VARS
 struct ButexWaiterCount : public bvar:Adder<int64_t>{
   ButexWaiterCount() : bvar::Adder<int64_t>("bthread_butex_waiter_count"){}
 };

 inline bvar::Adder<int64_t>& butex_waiter_count(){
   return *butil::get_leaky_singleton<ButexWaiterCount>();
 }
#endif

// If a thread would suspend for less than so many microseconds, return
// ETIMEDOUT directly.
// Use 1: sleeping for less than 2 microsecond is inefficient and useless.
static const int64_t MIN_SLEEP_US = 2;

 enum WaiterState{
   WAITER_STATE_NONE,
   WAITER_STATE_READY,
   WAITER_STATE_TIMEOUT,
   WAITER_STATE_UNMATCHEDVALUE,
   WAITER_STATE_INTERRUPTED,
 };

 struct Butex;

 struct ButexWaiter : public butil::LinkNode<ButexWaiter>{
   // tids of pthread are 0
   bthread_t tid;

   // Erasing node from middle of LinkedList is thread-unsafe, we need
   // to hold its container's lock.
   butil::atomic<Butex*> container;
 };

 // non_pthread_task allocates this structure on stack and queue it in
 // Butex::waiters.
 struct ButexBthreadWaiter : public ButexWaiter{
   TaskMeta* task_meta;
   TimerThread::TaskId sleep_id;
   WaiterState waiter_state;
   int expected_value;
   Butex* initial_butex;
   TaskControl* control;
 };

 // pthread_task or main_task allocates this structure on stack and queue it
 // in Butex::waiters.
 struct ButexPthreadWaiter : public ButexWaiter{
   butil::atomic<int> sig;
 };

 typedef butil::LinkedList<ButexWaiter> ButexWaiterList;

 enum ButexPthreadSignal{
   PTHREAD_NOT_SIGNALLED,
   PTHREAD_SIGNALLED
 };

 struct BAIDU_CACHELINE_ALIGNMENT Butex{
   Butex(){}
   ~Butex(){}

   butil::atomic<int> value;
   ButexWaiterList waiters;
   internal::FastPthreadMutex waiter_lock;
 };

 BAIDU_CASSERT(offsetof(Butex, value) == 0, offsetof_value_must_0);
 BAIDU_CASSERT(sizefof(Butex) == BAIDU_CACHELINE_SIZE, butex_fits_in_one_cacheline);

 static void wakeup_pthread(ButexPthreadWaiter* pw){
   // release fence makes wait_pthread see changes before wakeup.
   pw->sig.store(PTHREAD_SIGNALLED, butil::memory_order_release);
   // At this point, wait_pthread() possibly has woken up and destroyed `pw'.
   // In which case, futex_wake_private() should return EFAULT.
   // If crash happens in future, `pw' can be made TLS and never destroyed
   // to solve the issue.
   futex_wake_private(&pw->sig, 1);
 }

 bool erase_from_butex(ButexWaiter*， bool, WaiterState);

 int wait_pthread(ButexPthreadWaiter& pw, timespec* ptimeout){
   while(true){
     const int rc = futex_wait_private(&pw.sig, PTHREAD_NOT_SIGNALLED, ptimeout);
     if (PTHREAD_NOT_SIGNALLED != pw.sig.load(butil::memory_order_acquire)){
       // If `sig' is changed, wakeup_pthread() must be called and `pw'
       // is already removed from the butex.
       // Acquire fence makes this thread sees changes before wakeup.
       return rc;
     }
     if (rc != 0 && errno == ETIMEOUT){
       // Note that we don't handle the EINTR from futex_wait here since
       // pthreads waiting on a butex should behave similarly as bthreads
       // which are not able to be woken-up by signals.
       // EINTR on butex is only producible by TaskGroup::interrupt().

       // `pw' is still in the queue, remove it.
       if (!erase_from_butex(&pw, false, WAITER_STATE_TIMEOUT)){
         // Another thread is erasing `pw' as well, wait for the signal.
         // Acquire fence makes this thread sees changes before wakeup.
         if (pw.sig.load(butil::memory_order_acquire) == PTHREAD_NOT_SIGNALLED){
           ptimeout = NULL; // already timeout, ptimeout is expired.
           continue;
         }
       }
       return rc;
     }
   }
 }


 extern BAIDU_THREAD_LOCAL TaskGroup* tls_task_group;


}