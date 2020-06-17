//
// Created by dengxin05 on 2020/6/16.
//

#include "butil/scoped_lock.h"            // BAIDU_SCOPED_LOCK
#include "butil/errno.h"                  // berror
#include "butil/logging.h"
#include "butil/third_party/murmurhash3/murmurhash3.h"
#include "bthread/sys_futex.h"            // futex_wake_private
#include "bthread/interrupt_pthread.h"
#include "bthread/processor.h"            // cpu_relax
#include "bthread/task_group.h"           // TaskGroup
#include "bthread/task_control.h"
#include "bthread/timer_thread.h"         // global_timer_thread
#include <gflags/gflags.h>
#include "bthread/log.h"

DEFINE_int32(task_group_delete_delay, 1, "delay deletion of TaskGroup for so many seconds");
DEFINE_int32(task_group_runqueue_capacity, 4096, "capacity of runqueue in each TaskGroup");
DEFINE_int32(task_group_yield_before_idle, 0, "TaskGroup yields so many times before idle");

namespace bthread{
DECLARE_int32(bthread_concurrency);
DECLARE_int32(bthread_min_concurrency);

extern pthread_mutex_t g_task_control_mutex;
extern BAIDU_THREAD_LOCAL TaskGroup* tls_task_group;
void (*g_worker_startfn)() = NULL;

// May be called in other modules to run startfn in non-worker pthreads.
void run_worker_startfn(){
  if(g_worker_startfn){
    g_worker_startfn();
  }
}

void* TaskControl::worker_thread(void* arg){
  run_worker_startfn();
  #ifdef BAIDU_INTERNAL
  logging::ComlogInitializer comlog_initializer;
  #endif

  TaskControl* c = static_cast<TaskControl*>(arg);
  TaskGroup* g = c->create_group();
  TaskStatistics stat;
  if (NULL == g){
    LOG(ERROR) << "Fail to create TaskGroup in pthread=" << pthread_self();
    return NULL;
  }
  BT_VLOG << "Created worker=" << pthread_self() << " bthread=" << g->main_tid();

  tls_task_group = g;
  c->_nworker << 1;
  g->run_main_task();

  stat = g->main_stat();
  BT_VLOG << "Destroying woker=" << pthread_self() << " bthread="
          << g->main_tid() << " idle=" << stat.cputime_ns / 1000000.0
          << "ms uptime=" << g->current_uptime_ns() / 1000000.0 << "ms";
  tls_task_group = NULL;
  g->destroy_self();
  c->_nworkers << -1;
  return NULL;
}

TaskGroup* TaskControl::create_group(){
  TaskGroup* g = new (std::nothrow) TaskGroup(this);
  if (NULL == g){
    LOG(FATAL) << "Fail to new TaskGroup";
    return NULL;
  }
  if (g->init(FLAGS_task_group_runqueue_capacity) != 0){
    LOG(ERROR) << "Fail to init TaskGroup";
    delete g;
    return NULL;
  }
  if (_add_group(g) != 0){
    delete g;
    return NULL;
  }
  return g;
}

static void print_rq_sizes_in_the_tc(std::ostream &os, void *arg){
  TaskGroup *tc = (TaskGroup *)arg;
  tc->print_rq_sizes(os);
}

static double get_cumulated_worker_time_from_this(void* arg){
  return static_cast<TaskControl*>(arg)->get_cumulated_worker_time();
}

static int64_t get_cumulated_switch_count_from_this(void* arg){
  return static_cast<TaskControl*>(arg)->get_cumulated_switch_count();
}

static int64_t get_cumulated_signal_count_from_this(void* arg){
  return static_cast<TaskControl*>(arg)->get_cumulated_signal_count();
}

} // namespace bthread