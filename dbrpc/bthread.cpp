//
// Created by dengxin05 on 2020/6/8.
//

#include <gflag/gflags.h>
#include "butil/macros.h"                 // BAIDU_CASSERT
#include "butil/logging.h"
#include "bthread/task_group.h"           // TaskGroup
#include "bthread/task_control.h"         // TaskControl
#include "bthread/timer_thread.h"
#include "bthread/list_of_abafree_id.h"
#include "bthread/bthread.h"

namespace bthread{
DEFINE_int32(bthread_concurrency, 8 + BTHREAD_EPOLL_THREAD_NUM, "Number of pthread workers");
DEFINE_int32(bthread_min_concurrency, 0, "Initial number of pthread workers which will be added on-demand."
                                         " The laziness is disabled when this value is non-positive,"
                                         " and workers will be created eagerly according to -bthread_concurrency and bthread_setconcurrency().");

static bool never_set_bthread_concurrency = true;

static bool validate_bthread_concurrency(const char*, int32_t val){
  // bthread_setconcurrency sets the flag on success path which should
  // not be strictly in a validtor. But it's OK for a int flag.
  return bthread_setconcurrency(val) == 0;
}

const int ALLOW_UNUSED register_FLAGS_bthread_concurrency = ::GFLAGS_NS::RegisterFlagValidator(
    &FLAGS_bthread_concurrency,
    validate_bthread_concurrency);


}