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

static bool validate_bthread_min_concurrency(const char*, int32_t val);

const int ALLOW_UNUSED register_FLAGS_bthread_min_concurrency =
        ::GFLAGS_NS::RegisterFlagValidator(&FLAGS_bthread_min_concurrency, validate_bthread_min_concurrency);

BAIDU_CASSERT(sizeof(TaskControl*) == sizeof(butil::atomic<TaskControl*>), atomic_size_match);

pthread_mutex_t g_task_control_mutex = PTHREAD_MUTEX_INITIALIZER;
// Referenced in rpc, needs to be extern.
// Notice that we can't declare the variable as atomic<TaskControl*> which
// are not constructed before main().
TaskControl* g_task_control = NULL;

extern BAIDU_THREAD_LOCAL TaskGroup* tls_task_group;
extern void (*g_worker_startfn)();

inline TaskControl* get_task_control(){
    return g_task_control;
}

inline TaskControl* get_or_new_task_control(){
    butil::atomic<TaskControl*>* p = (butil::atomic<TaskControl*>*)&g_task_control;
    TaskControl* c = p->load(butil::memory_order_consume);
    if (c != NULL){
        return c;
    }
    BAIDU_SCOPED_LOCK(g_task_control_mutex);
    c = p->load(butil::memory_order_consume);
    if( c != NULL){
        return c;
    }
    c = new (std::nothrow) TaskControl;
    if (NULL == c){
        return NULL;
    }
    int concurrency = FLAGS_bthread_min_concurrency > 0 ?
            FLAGS_bthread_min_concurrency :
            FLAGS_bthread_concurrency;
    if (c->init(concurrency) != 0){
        LOG(ERROR) << "Fail to init g_task_control";
        delete c;
        return NULL;
    }
    p->store(c, butil::memory_order_release);
    return c;
}

static bool validate_bthread_min_concurrency(const char*, int32_t val){
    if (val <= 0){
        return true;
    }
    if (val < BTHREAD_MIN_CONCURRENCY || val > FLAGS_bthread_concurrency){
        return false;
    }
    TaskControl* c = get_task_control();
    if(!c){
        return true;
    }
    BAIDU_SCOPED_LOCK(g_task_control_mutex);
    int concurrency = c->concurrency();
    if (val > concurrency){
        int added = c->add_workers(val - concurrency);
        return added == (val - concurrency);
    }else{
        return true;
    }
}

__thread TaskGroup* tls_task_group_nosignal = NULL;

BUTIL_FORCE_INLINE int
start_from_non_worker(bthread_t* __restrict tid,
        const bthread_attr_t* __restrict attr,
        void* (*fn)(void*),
        void* __restrict arg){
    TaskControl* c = get_or_new_task_control();
    if(NULL == c){
        return ENOMEM;
    }
    if (attr != NULL && (attr->flags & BTHREAD_NOSIGNAL)){
        // Remember the TaskGroup to insert NOSIGNAL tasks for 2 reasons:
        // 1. NOSIGNAL is often for creating many bthreads in batch,
        //    inserting into the same TaskGroup maximizes the batch.
        // 2. bthread_flush() needs to know which TaskGroup to flush.
        TaskGroup* g = tls_task_group_nosignal;
        if (NULL == g){
            g = c->choose_one_group();
            tls_task_group_nosignal = g;
        }
        return g->start_backgroup<true>(tid, attr, fn, arg);
    }
    return c->choose_one_group()->start_backgroup<true>(tid, attr, fn, arg);
}

struct TidTraits{
    static const size_t BLOCK_SIZE = 63;
    static const size_t MAX_ENTRIES = 65536;
    static const bthread_t ID_INIT;
    static bool exists(bthread_t id){
        return bthread::TaskGroup::exists(id);
    }
};
const bthread_t TidTraits::ID_INIT = INVALID_BTHREAD;

typedef ListOfABAFreeId<bthread_t, TidTraits> TidList;

struct TidStopper{
    void operator()(bthread_t id)const{
        bthread_stop(id);
    }
};
struct TidJoiner{
    void operator()(bthread_t & id)const{
        bthread_join(id, NULL);
        id = INVALID_BTHREAD;
    }
};

} // namespace bthread

extern "C" {
    int bthread_start_urgent(bthread_t* __restrict tid,
            const bthread_attr_t* __restrict attr,
            void* (*fn)(void*),
            void* __restrict arg){
        bthread::TaskGroup* g = bthread::tls_task_group;
        if (g){
            // start from worker
            return bthread::TaskGroup::start_foreground(&g, tid, attr, fn, arg);
        }
        return bthread::start_from_non_worker(tid, attr, fn, arg);
    }

    int bthread_start_background(bthread_t* __restrict tid,
            const bthread_attr_t* __restrict attr,
            void* (*fn)(void*),
            void* __restrict arg){
        bthread::TaskGroup* g = bthread::tls_task_group;
        if (g){
            // start from worker
            return g->start_backgroup<false>(tid, attr, fn, arg);
        }
        return bthread::start_from_non_worker(tid, attr, fn, arg);
    }

    void bthread_flush(){
        bthread::TaskGroup* g = bthread::tls_task_group;
        if(g){
            return g->flush_nosignal_tasks();
        }

        g = bthread::tls_task_group_nosignal;
        if (g){
            // NOSIGNAL tasks were created in this non-worker.
            bthread::tls_task_group_nosignal = NULL;
            return g->flush_nosignal_tasks_remote();
        }
    }

    int bthread_interrupt(bthread_t tid){
        return bthread::TaskGroup::interrupt(tid, bthread::get_task_control());
    }
}

