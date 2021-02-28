# Work Stealing以及任务的执行与切换
## bthread是协程吗？
如果你使用过brpc，那么对bthread应该并不陌生。毫不夸张地说，brpc的精华全在bthread上了。bthread可以理解为“协程”，尽管官方文档的FAQ中，并不称之为协程。

若说到pthread大家都不陌生，是POSIX标准中定义的一套线程模型。应用于Unix Like系统，在Linux上pthread API的具体实现是NPTL库实现的。在Linux系统上，其实没有真正的线程，其采用的是LWP（轻量级进程）实现的线程。而bthread是brpc实现的一套“协程”，当然这并不是传统意义上的协程。就像1个进程可以开辟N个线程一样。传统意义上的协程是一个线程中开辟多个协程，也就是通常意义的N:1协程。比如微信开源的libco就是N:1的，libco属于非对称协程，区分caller和callee，而bthread是M:N的“协程”，每个bthread之间的平等的，所谓的M:N是指协程可以在线程间迁移。熟悉Go语言的朋友，应该对goroutine并不陌生，它也是M:N的。

当然准确的说法goroutine也并不等同于协程。不过由于通常也称goroutine为协程，从此种理解上来讲，bthread也可算是协程，只是不是传统意义上的协程！当然，咬文嚼字，没必要。

要实现M:N其中关键就是：工作窃取（Work Stealing）算法。不过在真正展开介绍工作窃取之前，我们先透视一下bthread的组成部分。

## bthread的三个T
讲到bthread，首先要讲的三大件：TaskControl、TaskGroup、TaskMeta。以下简称TC、TG、TM。

TaskControl进程内全局唯一。TaskGroup和线程数相当，每个线程(pthread）都有一个TaskGroup，brpc中也将TaskGroup称之为 worker。而TM基本就是表征bthread上下文的真实结构体了。

虽然前面我们说bthread并不严格从属于一个pthread，但是bthread在运行的时候还是需要在一个pthread中的worker中（也即TG）被调用执行的。

## 从bthread_start_background讲到TM
以下开启Hard模式，我们先从最常见的bthread_start_background来导入。bthread_start_background是我们经常使用的创建bthread任务的函数，源码如下：
```
int bthread_start_background(bthread_t* __restrict tid,
                             const bthread_attr_t* __restrict attr,
                             void * (*fn)(void*),
                             void* __restrict arg) {
    bthread::TaskGroup* g = bthread::tls_task_group;
    if (g) {
        // start from worker
        return g->start_background<false>(tid, attr, fn, arg);
    }
    return bthread::start_from_non_worker(tid, attr, fn, arg);
}
```

函数接口十分类似于pthread 的pthread_create()。也就是设置bthread的回调函数及其参数。

如果能获取到thread local的TG（tls_task_group），那么直接用这个tg来运行任务：start_background()。

看下start_background源码，代码不少，但可以一眼略过。

```
template <bool REMOTE>
int TaskGroup::start_background(bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
    if (__builtin_expect(!fn, 0)) {
        return EINVAL;
    }
    const int64_t start_ns = butil::cpuwide_time_ns();
    const bthread_attr_t using_attr = (attr ? *attr : BTHREAD_ATTR_NORMAL);
    butil::ResourceId<TaskMeta> slot;
    TaskMeta* m = butil::get_resource(&slot);
    if (__builtin_expect(!m, 0)) {
        return ENOMEM;
    }
    CHECK(m->current_waiter.load(butil::memory_order_relaxed) == NULL);
    m->stop = false;
    m->interrupted = false;
    m->about_to_quit = false;
    m->fn = fn;
    m->arg = arg;
    CHECK(m->stack == NULL);
    m->attr = using_attr;
    m->local_storage = LOCAL_STORAGE_INIT;
    m->cpuwide_start_ns = start_ns;
    m->stat = EMPTY_STAT;
    m->tid = make_tid(*m->version_butex, slot);
    *th = m->tid;
    if (using_attr.flags & BTHREAD_LOG_START_AND_FINISH) {
        LOG(INFO) << "Started bthread " << m->tid;
    }
    _control->_nbthreads << 1;
    if (REMOTE) {
        ready_to_run_remote(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    } else {
        ready_to_run(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    }
    return 0;
}
```

主要就是从资源池取出一个TM对象m，然后对他进行初始化，将回调函数fn赋进去，将fn的参数arg赋进去等等。

另外就是调用make_tid，计算出了一个tid，这个tid会作为出参返回，也会被记录到TM对象中。tid可以理解为一个bthread任务的任务ID号。类型是bthread_t，其实bthread_t只是一个uint64_t的整型，make_tid的计算生成逻辑如下：

```
inline bthread_t make_tid(uint32_t version, butil::ResourceId<TaskMeta> slot) {
    return (((bthread_t)version) << 32) | (bthread_t)slot.value;
}
```
version是即TM的成员version_butex（uint32_t*)解引用获得的一个版本号。在TM构造函数内被初始化为1，且brpc保证其用不为0。在TG的相关逻辑中会修改这个版本号。

## TaskControl
先看TC是如何被创建的，TC对象的直接创建（new和初始化）是在get_or_new_task_control()中，这个函数顾名思义，就是获取TC，有则返之，无则造之。所以TC是进程内全局唯一的，也就是只有一个实例。而TG和TM是一个比一个多。

下面展示一下**get_or_new_task_control的被调用链**（表示函数的被调用关系），也就能更直观的发现TC是如何被创建的。

* get_or_new_task_control
    * start_from_non_worker
        * bthread_start_background
        * bthread_start_urgent
    * bthread_timer_add
    
我们通常用的bthread_start_background()或者定期器bthread_timer_add()都会调用到get_or_new_task_control。

回顾一下bthread_start_background的定义：
```
int bthread_start_background(bthread_t* __restrict tid,
                             const bthread_attr_t* __restrict attr,
                             void * (*fn)(void*),
                             void* __restrict arg) {
    bthread::TaskGroup* g = bthread::tls_task_group;
    if (g) {
        // start from worker
        return g->start_background<false>(tid, attr, fn, arg);
    }
    return bthread::start_from_non_worker(tid, attr, fn, arg);
}
```

如果能获取到thread local的TG，那么直接用这个tg来运行任务。如果获取不到TG，则说明当前还没有bthread的上下文（TC、TG都没有），所以调用start_from_non_worker()函数，其中再调用get_or_new_task_control()函数，从而创建出一个TC来。

get_or_new_task_control()的基本逻辑，直接看代码，主要地方我补充了注释：
```
inline TaskControl* get_or_new_task_control() {
    // 1. 全局变量TC（g_task_control）初始化原子变量
    butil::atomic<TaskControl*>* p = (butil::atomic<TaskControl*>*)&g_task_control;
    // 2.1 通过原子变量进行load，取出TC指针，如果不为空，直接返回
    TaskControl* c = p->load(butil::memory_order_consume);
    if (c != NULL) {
        return c;
    }
    // 2.2. 竞争加自旋锁，重复上一操作
    BAIDU_SCOPED_LOCK(g_task_control_mutex);
    c = p->load(butil::memory_order_consume);
    if (c != NULL) {
        return c;
    }
    
    // 2. 走到这，说明TC确实为NULL，开始new一个
    c = new (std::nothrow) TaskControl;
    if (NULL == c) {
        return NULL;
    }
    // 3. 用并发度concurrency来初始化全局TC
    int concurrency = FLAGS_bthread_min_concurrency > 0 ?
        FLAGS_bthread_min_concurrency :
        FLAGS_bthread_concurrency;
    if (c->init(concurrency) != 0) {
        LOG(ERROR) << "Fail to init g_task_control";
        delete c;
        return NULL;
    }

    // 4. 将全局TC存入原子变量中
    p->store(c, butil::memory_order_release);
    return c;
}
```
串完上述逻辑，我们来关注一下TC的初始化操作：TaskControl::init()

## TaskControl::init()
源码十分简单：
```
   for (int i = 0; i < _concurrency; ++i) {
        const int rc = pthread_create(&_workers[i], NULL, worker_thread, this);
        if (rc) {
            LOG(ERROR) << "Fail to create _workers[" << i << "], " << berror(rc);
            return -1;
        }
    }
```
所谓的TC的初始化，就是调用了pthread_create()创建了N个新线程。N就是前面提到的并发度：concurrency。每个pthread线程的回调函数为worker_thread，通过这个函数也便引出了本文真正的主角TaskGroup了。

## TaskControl::worker_thread()
毋庸置疑，这是一个static函数（回调函数一般为static）。在这个函数中会创建了一个TaskGroup。去掉一些日志逻辑，我们来看下源码，请大家关注我加的注释部分：

```
void* TaskControl::worker_thread(void* arg) {
    // 1. TG创建前的处理，里面也是回调g_worker_start_fun函数来执行操作，
    // 可以通过 bthread_set_worker_startfn() 来设置这个回调函数，
    // 实际但是很少用到这个功能
    run_worker_startfn();    

    // 2. 获取TC的指针
    TaskControl* c = static_cast<TaskControl*>(arg);
    // 2.1 创建一个TG
    TaskGroup* g = c->create_group();
    TaskStatistics stat;
    if (NULL == g) {
        LOG(ERROR) << "Fail to create TaskGroup in pthread=" << pthread_self();
        return NULL;
    }

    // 3.1 把thread local的tls_task_group 用刚才创建的TG来初始化
    tls_task_group = g;
    // 3.2 worker计数加1（_nworkers是bvar::Adder<int64_t>类型)
    c->_nworkers << 1;

    // 4. TG运行主任务（死循环）
    g->run_main_task();

    // 5. TG结束时返回状态信息，后面其实有输出stat的日志，这里去掉了
    stat = g->main_stat();
    // ...
  
    // 6. 各种清理操作
    tls_task_group = NULL;
    g->destroy_self();
    c->_nworkers << -1;
    return NULL;
}
```

通过这个函数，我们的观察视角也就从TC平稳的过渡到TG了。其中TG最主要的函数就是run_main_task()。而你们心心念念的work stealing也不远了。

## TaskGroup
### TG的主要成员
讲到TG先看TG的主要成员：
```
    size_t _steal_seed;
    size_t _steal_offset;
    ContextualStack* _main_stack;
    bthread_t _main_tid;
    WorkStealingQueue<bthread_t> _rq;
    RemoteTaskQueue _remote_rq;
```
每个TG都维护自己一个单独的栈指针：_main_stack和_main_tid。也就是是说TG中有一个特殊的TM。我姑且称之为“主TM”。这两个是在TG初始化的时候赋值的。

每个TG有两个TM的队列，它们之间有啥区别呢？

通过在代码里搜索这两个队列入队的逻辑，可以发现。当调用bthread_start_background()创建bthread任务时，其内部会继续调用TG的ready_to_run()，接着push_rq()函数，给TG的rq入队。而remote_rq队列的入队是是通过执行TG的ready_to_run_remote()完成的。

再看一下ready_to_run_remote注释：

```
    // Push a bthread into the runqueue from another non-worker thread.
    void ready_to_run_remote(bthread_t tid, bool nosignal = false);
```
在没有woker（TG）的线程中把bthread入队，只能入到有worder线程中的TG的remote_rq队列。

再看下ready_to_run_remote()的调用的地方。

在butex_wake()中：

```
    TaskGroup* g = tls_task_group;
    if (g) {
        TaskGroup::exchange(&g, bbw->tid);
    } else {
        bbw->control->choose_one_group()->ready_to_run_remote(bbw->tid);
    }
```
在start_background()中：
```
template <bool REMOTE>
int TaskGroup::start_background(bthread_t* __restrict th,
                                const bthread_attr_t* __restrict attr,
                                void * (*fn)(void*),
                                void* __restrict arg) {
...
...
    if (REMOTE) {
        ready_to_run_remote(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    } else {
        ready_to_run(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    }
    return 0;
}
```

start_background<true>的时候会调用ready_to_run_remote。在start_from_non_worker()中，会start_background<true>。

好了，言归正传。

## TaskGroup::run_main_task()
run_main_task()，去掉一些bvar相关的代码，这个函数也异常简洁。

```
void TaskGroup::run_main_task() {
    ...
    TaskGroup* dummy = this;
    bthread_t tid;
    while (wait_task(&tid)) {
        TaskGroup::sched_to(&dummy, tid);
        DCHECK_EQ(this, dummy);
        DCHECK_EQ(_cur_meta->stack, _main_stack);
        if (_cur_meta->tid != _main_tid) {
            TaskGroup::task_runner(1/*skip remained*/);
        }
        ...
    }
    // Don't forget to add elapse of last wait_task.
    current_task()->stat.cputime_ns += butil::cpuwide_time_ns() - _last_run_ns;
}
```

死循环执行wait_task来等待有效的任务，如果能等到任务，wait_task的出参tid（bthread_t类型）会记录这个任务的ID号。好了，拿到任务ID号tid后，执行sched_to函数来切换栈。在进行了一些check工作后，判断如果当前的tid不是TG的主要tid(main_tid)则执行：TaskGroup::task_runner(1);

三个关键函数：wait_task、sched_to、task_runner。

1. wait_task：找到一个任务。其中会涉及工作窃取（work stealing）；
2. sched_to：进行栈、寄存器等运行时上下文的切换，为接下来运行的任务恢复其上下文；
3. task_runner：执行任务。
现在我们可以通过现在的视角切入到“work stealing”了。

## 工作窃取（work stealing）
work stealing不是协程的专利，更不是Go语言的专利。work stealing是一种通用的实现负载均衡的算法。这里的负载均衡指的不是像Nginx那种对于外部网络请求做负载均衡，此处指的是每个CPU处理任务时，每个核的负载均衡。不止协程，其实线程池也可以做work stealing。

## TaskGroup::wait_task()
```
// 简化起见，去掉了BTHREAD_DONT_SAVE_PARKING_STATE条件宏判断逻辑相关
bool TaskGroup::wait_task(bthread_t* tid) {
    do {
        if (_last_pl_state.stopped()) {
            return false;
        }
        _pl->wait(_last_pl_state);
        if (steal_task(tid)) {
            return true;
        }
    } while (true);
}
```

_pl是ParkingLot*类型，_last_plstate是pl中的state。

_pl->wait(_last_pl_state)内部调用的futex做的wait操作，这里可以简单理解为阻塞等待被通知来终止阻塞，当阻塞结束之后，执行steal_task()来进行工作窃取。如果窃取成功则返回。

## TaskGoup::steal_task()
```
    bool steal_task(bthread_t* tid) {
        if (_remote_rq.pop(tid)) {
            return true;
        }
        _last_pl_state = _pl->get_state();
        return _control->steal_task(tid, &_steal_seed, _steal_offset);
    }
```

首先TG的remote_rq队列中的任务出队，如果没有则同全局TC来窃取任务。

TC的steal_task实现如下：

## TaskControl::steal_task()
```
bool TaskControl::steal_task(bthread_t* tid, size_t* seed, size_t offset) {
    // 1: Acquiring fence is paired with releasing fence in _add_group to
    // avoid accessing uninitialized slot of _groups.
    const size_t ngroup = _ngroup.load(butil::memory_order_acquire/*1*/);
    if (0 == ngroup) {
        return false;
    }

    // NOTE: Don't return inside `for' iteration since we need to update |seed|
    bool stolen = false;
    size_t s = *seed;
    for (size_t i = 0; i < ngroup; ++i, s += offset) {
        TaskGroup* g = _groups[s % ngroup];
        // g is possibly NULL because of concurrent _destroy_group
        if (g) {
            if (g->_rq.steal(tid)) {
                stolen = true;
                break;
            }
            if (g->_remote_rq.pop(tid)) {
                stolen = true;
                break;
            }
        }
    }
    *seed = s;
    return stolen;
}
```
可以看出是随机找一个TG，先从它的rq队列窃取任务，如果失败再从它的remote_rq队列窃取任务。在消费的时候rq比remote_rq有更高的优先级，显而易见，我们一定是想先执行有woker的线程自己push到队列中的bthread，然后再消费其他线程push给自己的bthread。

通过上面三个函数可以看出TaskGroup::wait_task() 在等待任务的时候，是优先获取当前TG的remote_rq，然后是依次窃取其他TG的rq、remote_rq。它并没有从当前TG的rq找任务！这是为什么呢？原因是避免race condition。也就是避免多个TG 等待任务的时候，当前TG从rq取任务，与其他TG过来自己这边窃取任务造成竞态。从而提升一点点的性能。

那么当前TG的rq是什么时候被消费的呢？

在TG的ending_sched()函数中有rq的出队操作，而ending_sched()在t**ask_runner中被调用，task_runner也是**run_main_task()的三个关键函数之一。

## TaskGroup::task_runner()
```
void TaskGroup::task_runner(intptr_t skip_remained) {
    TaskGroup* g = tls_task_group;

    if (!skip_remained) {
        while (g->_last_context_remained) {
            RemainedFn fn = g->_last_context_remained;
            g->_last_context_remained = NULL;
            fn(g->_last_context_remained_arg);
            g = tls_task_group;
        }
     ...
     }
```
在run_main_task()中task_runner()的输入参数是1，所以上面的if逻辑会被跳过。这里忽略这个if，继续向下看，下面是一个很长的do-while循环(去掉一些日志和bvar相关逻辑，补充注释）：
```
    do {
        // Meta and identifier of the task is persistent in this run.
        TaskMeta* const m = g->_cur_meta;
        ... 
        // 执行TM（bthread)中的回调函数
        void* thread_return;
        try {
            thread_return = m->fn(m->arg);
        } catch (ExitException& e) {
            thread_return = e.value();
        }

        // Group is probably changed
        g = tls_task_group;

        // TODO: Save thread_return
        (void)thread_return;

        ... 日志

        // 清理 线程局部变量（下面是原注释)
        // Clean tls variables, must be done before changing version_butex
        // otherwise another thread just joined this thread may not see side
        // effects of destructing tls variables.
        KeyTable* kt = tls_bls.keytable;
        if (kt != NULL) {
            return_keytable(m->attr.keytable_pool, kt);
            // After deletion: tls may be set during deletion.
            tls_bls.keytable = NULL;
            m->local_storage.keytable = NULL; // optional
        }

        // 累加版本号，且版本号不能为0（下面是原注释)
        // Increase the version and wake up all joiners, if resulting version
        // is 0, change it to 1 to make bthread_t never be 0. Any access
        // or join to the bthread after changing version will be rejected.
        // The spinlock is for visibility of TaskGroup::get_attr.
        {
            BAIDU_SCOPED_LOCK(m->version_lock);
            if (0 == ++*m->version_butex) {
                ++*m->version_butex;
            }
        }
        // 唤醒joiner
        butex_wake_except(m->version_butex, 0);

        // _nbthreads减1（注意_nbthreads不是整型）
        g->_control->_nbthreads << -1;
        g->set_remained(TaskGroup::_release_last_context, m);

        // 查找下一个任务，并切换到其对应的运行时上下文
        ending_sched(&g);

    } while (g->_cur_meta->tid != g->_main_tid); 
```
do while循环中会执行回调函数，结束的时候会查找下一个任务，并切换上下文。循环的终止条件是tls_task_group的_cur_meta不等于其_main_tid。

在ending_sched()中，会有依次从TG的rq、remote_rq取任务，找不到再窃取其他TG的任务，如果都找不到任务，则设置_cur_meta为_main_tid，也就是让task_runner()的循环终止。

然后就会回到run_main_task()的主循环，继续wait_task()等待新任务了。

好了，run_main_task()的三大关键函数，已过其二，还剩下一个sched_to()还未揭开其庐山真面，之所以把它放到最后讲，是因为会涉及一些汇编的知识，读起来可能晦涩艰深，我也没有把握讲好。

## TaskGroup::sched_to()
```
inline void TaskGroup::sched_to(TaskGroup** pg, bthread_t next_tid) {
    TaskMeta* next_meta = address_meta(next_tid);
    if (next_meta->stack == NULL) {
        ContextualStack* stk = get_stack(next_meta->stack_type(), task_runner);
        if (stk) {
            next_meta->set_stack(stk);
        } else {
            // stack_type is BTHREAD_STACKTYPE_PTHREAD or out of memory,
            // In latter case, attr is forced to be BTHREAD_STACKTYPE_PTHREAD.
            // This basically means that if we can't allocate stack, run
            // the task in pthread directly.
            next_meta->attr.stack_type = BTHREAD_STACKTYPE_PTHREAD;
            next_meta->set_stack((*pg)->_main_stack);
        }
    }
    // Update now_ns only when wait_task did yield.
    sched_to(pg, next_meta);
}
```
通过传入的参数：next_tid找到TM：next_meta，和对应的ContextualStack信息：stk。

然后给next_meta设置栈stk。

最后调用另外一个重载的sched_to：
```
void TaskGroup::sched_to(TaskGroup** pg, TaskMeta* next_meta);
```
源码：
```
void TaskGroup::sched_to(TaskGroup** pg, TaskMeta* next_meta) {
    TaskGroup* g = *pg;

    // Save errno so that errno is bthread-specific.
    const int saved_errno = errno;
    void* saved_unique_user_ptr = tls_unique_user_ptr;

    TaskMeta* const cur_meta = g->_cur_meta;
    const int64_t now = butil::cpuwide_time_ns();
    const int64_t elp_ns = now - g->_last_run_ns;
    g->_last_run_ns = now;
    cur_meta->stat.cputime_ns += elp_ns;
    if (cur_meta->tid != g->main_tid()) {
        g->_cumulated_cputime_ns += elp_ns;
    }
    ++cur_meta->stat.nswitch;
    ++ g->_nswitch;
```

记录一些数据。继续看代码，判断下一个的TM（next_meta）和当前TM（cur_meta）如果不是同一个，就去切换栈。

```
 // Switch to the task
    if (__builtin_expect(next_meta != cur_meta, 1)) {
        g->_cur_meta = next_meta;
        // Switch tls_bls
        cur_meta->local_storage = tls_bls;
        tls_bls = next_meta->local_storage;


        if (cur_meta->stack != NULL) {
            if (next_meta->stack != cur_meta->stack) {
                jump_stack(cur_meta->stack, next_meta->stack);
                // probably went to another group, need to assign g again.
                g = tls_task_group;
            }

        }
        // else because of ending_sched(including pthread_task->pthread_task)
    } else {
        LOG(FATAL) << "bthread=" << g->current_tid() << " sched_to itself!";
    }
```

tls_bls表示的是TM（bthread）内的局部存储。先做还原，并且赋值成下一个TM的局部存储。接着执行jump_stack()去切换栈。

上面的大if结束之后，去执行TG的remain回调函数（如果设置过）。

```
    while (g->_last_context_remained) {
        RemainedFn fn = g->_last_context_remained;
        g->_last_context_remained = NULL;
        fn(g->_last_context_remained_arg);
        g = tls_task_group;
    }

    // Restore errno
    errno = saved_errno;
    tls_unique_user_ptr = saved_unique_user_ptr;

    *pg = g;
```

## jump_stack()
src/bthread/stack_inl.h
```
inline void jump_stack(ContextualStack* from, ContextualStack* to) {
    bthread_jump_fcontext(&from->context, to->context, 0/*not skip remained*/);
}
```
bthread_jump_fcontext()其实是汇编函数，在bthread/context.cpp中，功能就是进行栈上下文的切换（跳转）。与之配套的还有一个bthread_make_fcontext()，负责创建bthread的栈上下文。这两个函数是实现栈上下文切换的核心。它们的代码其实并非brpc的原创，而是出自开源项目libcontext。libcontext是boost::context的简化实现。打开bthread/context.h可以看到版权声明：
```
/*

    libcontext - a slightly more portable version of boost::context

    Copyright Martin Husemann 2013.
    Copyright Oliver Kowalke 2009.
    Copyright Sergue E. Leontiev 2013.
    Copyright Thomas Sailer 2013.
    Minor modifications by Tomasz Wlostowski 2016.

 Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
            http://www.boost.org/LICENSE_1_0.txt)

*/
```

其实另外一个C++协程的开源项目libgo中的Context也脱胎于此。

在context.cpp中，定义了各种平台的bthread_jump_fcontext() /bthread_make_fcontext()实现。__asm代码块是C语言文件中编写汇编语言代码的写法。

```
#if defined(BTHREAD_CONTEXT_PLATFORM_linux_x86_64) && defined(BTHREAD_CONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl bthread_jump_fcontext\n"
".type bthread_jump_fcontext,@function\n"
".align 16\n"
"bthread_jump_fcontext:\n"
"    pushq  %rbp  \n"
"    pushq  %rbx  \n"
"    pushq  %r15  \n"
"    pushq  %r14  \n"
"    pushq  %r13  \n"
"    pushq  %r12  \n"
"    leaq  -0x8(%rsp), %rsp\n"
"    cmp  $0, %rcx\n"
"    je  1f\n"
"    stmxcsr  (%rsp)\n"
"    fnstcw   0x4(%rsp)\n"
"1:\n"
"    movq  %rsp, (%rdi)\n"
"    movq  %rsi, %rsp\n"
"    cmp  $0, %rcx\n"
"    je  2f\n"
"    ldmxcsr  (%rsp)\n"
"    fldcw  0x4(%rsp)\n"
"2:\n"
"    leaq  0x8(%rsp), %rsp\n"
"    popq  %r12  \n"
"    popq  %r13  \n"
"    popq  %r14  \n"
"    popq  %r15  \n"
"    popq  %rbx  \n"
"    popq  %rbp  \n"
"    popq  %r8\n"
"    movq  %rdx, %rax\n"
"    movq  %rdx, %rdi\n"
"    jmp  *%r8\n"
".size bthread_jump_fcontext,.-bthread_jump_fcontext\n"
".section .note.GNU-stack,\"\",%progbits\n"
);
```

这里的汇编是AT&T汇编，和Intel汇编语法不通。比如这里的mov操作，在从左到右看的。movq和popq的q表示操作的单位是四字（64位），如果是32位系统，则是movl和popl了。
```
    pushq  %rbp
    pushq  %rbx
    pushq  %r15
    pushq  %r14
    pushq  %r13
    pushq  %r12

```
常规操作，就是把函数调用方的相关寄存器入栈，也就是保存调用方的运行环境。在当前函数执行结束之后要从栈中还原数据到相应的寄存器中，从而让调用方继续执行。所以末尾有出栈操作。

在入栈之后：
```
leaq  -0x8(%rsp), %rsp
```
表示：rsp 栈顶寄存器下移 8 字节，为FPU 浮点运算预留。

另外值得一提的是bthread_jump_fcontext()函数在调用的时候是传入了3个参数，但是定义的bthread_jump_fcontext()是可以接收4个参数的。也正是因为这个第4个参数，导致了代码里有了2次跳转，分别跳转到1和2处。

先看一下函数参数和寄存器的关系：

%rdi	第1个参数
%rsi	第2个参数
%rdx	第3个参数
%rcx	第4个参数

在leaq指令之后，开始判断第四个参数的值。

```
    cmp  $0, %rcx
    je  1f
    stmxcsr  (%rsp)    // 保存当前MXCSR内容到rsp指向的位置
    fnstcw   0x4(%rsp) // 保存当前FPU状态字到rsp+4指向的位置
1:
```
如果第四个参数为0则直接跳转到1处。也就是跳过stmxcsr、fnstcw这两个指令。对于我们的场景而言，没有第四个参数也就不需要管这个。继续：

```
1:
    movq  %rsp, (%rdi)
    movq  %rsi, %rsp
```
我们知道%rdi和%rsi表示的是第一个参数和第二个参数，也就是：&from->context 和 to->context。

这两个movq指令表示的就是栈切换的核心操作，将当前的栈指针(%rsp)存储到第一个参数所指向的内存中。然后将第二个参数的值赋值给栈指针。修改栈指针，就是更改了栈顶，也就是进行了实际的栈切换操作。

接着是不太重要的代码，还是和第四个参数有关的：

```
    cmp  $0, %rcx
    je  2f
    ldmxcsr  (%rsp)
    fldcw  0x4(%rsp)
2:
```
也就是说如果第4个参数是0，则跳转到2。跳过的两条指令ldmxcsr、fldcw可以理解为是之前stmxcsr、fnstcw那两个指令的逆操作（也就是还原一下）。

```
2:
    leaq  0x8(%rsp), %rsp
```
%rsp 栈顶寄存器上移 8 字节，恢复为 FPU 浮点运算预留空间。

接着还原从栈中各个寄存器，因为是栈，所以逆向出栈。
```
    popq  %r12
    popq  %r13
    popq  %r14
    popq  %r15
    popq  %rbx
    popq  %rbp
```
在这6个popq之后还有一个popq，和前面的pushq是没有对应关系的。
```
  popq  %r8
```
是将bthread_jump_fcontext()之后该执行的指令地址，放到 %r8 寄存器中。展开一下谈谈，比如在函数A调用函数B的时候，会先把函数的返回值入栈，然后再把函数B的参数入栈。所以对应逆操作，在函数参数都出栈之后，继续出栈的数据就是函数的返回地址！
```
    movq  %rdx, %rax
    movq  %rdx, %rdi
```
%rdx表示的是函数的第三个参数，也就是是否：skip remained，当前都是0。先后存入到%rax和%rdi中。

%rax寄存器表示的是返回值。

%rdi表示的是函数第一个参数。也就是给切换完栈之后要调用的函数，准备参数。

```
   jmp  *%r8
```
跳转到返回地址，即调用方在调用完bthread_jump_fcontext()后，继续执行的指令位置。


[libgo 源码剖析（3. libgo上下文切换实现）](https://blog.51cto.com/muhuizz/2330678)

[brpc源码学习（二）-bthread的创建与切换_KIDGIN7439的专栏-CSDN博客](https://blog.csdn.net/KIDGIN7439/article/details/106426635)

# ParkingLot 与Worker同步任务状态
通过上一篇文章我们知道TaskGroup（以下简称TG）是在死循环等待任务，然后切换栈，接着去执行任务。在当前TG没有任务的时候会进行“工作窃取”（work stealing）窃取其他TG的任务。

当然任务并不一定一直有，而TG虽然是在死循环，却也不是在时刻不停地去check是否有任务。这样太消耗资源，它在没有任务的时候也会“休眠”，当有自己TG或其他TG出现任务的时候被唤醒然后去消费任务。

这个大体思路和线程中的**条件变量**类似。条件变量是线程间同步的一种方式。brpc的实现也是有类似的wait(阻塞并等待）和signal(通知并唤醒）的操作。而具体实现worker间的状态同步是通过ParkingLot这一类型来实现的。

先不看ParkingLot的定义，而是看一下ParkingLot与TaskControl（以下简称TC）与TaskGroup的关系。

## ParkingLot
ParkingLot直译就是停车场，以下**简称PL**。我们暂时先不展开PL的定义。

从TC切入。TC中有ParkingLot成员：
```
    static const int PARKING_LOT_NUM = 4;
    ParkingLot _pl[PARKING_LOT_NUM];
```
也就是说一个TC有4个PL对象。因为全局只有一个TC，所以也就是全局只有4个PL。

TG中也有PL相关的成员(BTHREAD_DONT_SAVE_PARKING_STATE是开启的）：

```
    ParkingLot* _pl;
#ifndef BTHREAD_DONT_SAVE_PARKING_STATE
    ParkingLot::State _last_pl_state;
#endif
```
_pl和_last_pl_state。_pl只是一个指针，其实他也源自TC中的pl。看TG的构造函数。
```
TaskGroup::TaskGroup(TaskControl* c)
... // 初始化列表，给成员赋值默认值，这里忽略
{
    _steal_seed = butil::fast_rand();
    _steal_offset = OFFSET_TABLE[_steal_seed % ARRAY_SIZE(OFFSET_TABLE)];
    _pl = &c->_pl[butil::fmix64(pthread_numeric_id()) % TaskControl::PARKING_LOT_NUM];
}
```

butil::fmix64()是一个hash函数，用的murmurhash的算法，将输入的整型映射成另外一个整型。这里用pthread线程的id作为参赛，进行hash，然后把结果再对PARKING_LOT_NUM取模。相当于是从TC的4个PL中选择了一个PL，赋值给了TG！

换言之，TC下面的所有TG（worker）被分成了4组，每组共享一个PL。通过PL在调控TG之间bthread任务的生产与消费。之所以用4个PL，而不是一个PL，大概率也是为了减少race condition（竞争状态）减少性能开销。

## 从生产者的角度出发
我们常用的bthread_start_background()会调用TG的start_background()。

**在TaskGroup::start_background()中的定义中有：**
```
   if (REMOTE) {
        ready_to_run_remote(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    } else {
        ready_to_run(m->tid, (using_attr.flags & BTHREAD_NOSIGNAL));
    }
```
ready_to_run_remote()和ready_to_run()的第二个参数nosignal，需要创建bthread任务的时候，给bthread设置属性：BTHREAD_NOSIGNAL。比如：
```
// 样例
bthread_t th;
bthread_attr_t tmp = BTHREAD_ATTR_NORMAL | BTHREAD_NOSIGNAL;
bthread_start_background(&th, &tmp, ProcessInputMessage, call_back_func);
```
不过通常我们调用bthread_start_background()的时候，第二个参数是设置为NULL的。所以可以暂时忽略nosignal相关逻辑。默认都是走signal的。注意这里的说的signal不是Unix C环境编程里面的信号。而是brpc自己给bthread实现的一套调控TG（worker）等待与唤醒的信号。

回看ready_to_run_remote()和ready_to_run()。ready_to_run()就是把任务入队到TG的 rq，ready_to_run_remote()是在当前线程不是brpc的worker()的时候（在worker外创建的 bthread任务），把任务通过TC入队到某个TG的 remote_rq。

ready_to_run()源码定义如下：
```
void TaskGroup::ready_to_run(bthread_t tid, bool nosignal) {
    push_rq(tid);
    if (nosignal) {
        ++_num_nosignal;
    } else {
        const int additional_signal = _num_nosignal;
        _num_nosignal = 0;
        _nsignaled += 1 + additional_signal;
        _control->signal_task(1 + additional_signal);
    }
}
```
ready_to_run()比较简洁，我们继续看下ready_to_run_remote()的定义：
```
void TaskGroup::ready_to_run_remote(bthread_t tid, bool nosignal) {
    _remote_rq._mutex.lock();
    while (!_remote_rq.push_locked(tid)) {
        flush_nosignal_tasks_remote_locked(_remote_rq._mutex);
        LOG_EVERY_SECOND(ERROR) << "_remote_rq is full, capacity="
                                << _remote_rq.capacity();
        ::usleep(1000);
        _remote_rq._mutex.lock();
    }
    if (nosignal) {
        ++_remote_num_nosignal;
        _remote_rq._mutex.unlock();
    } else {
        const int additional_signal = _remote_num_nosignal;
        _remote_num_nosignal = 0;
        _remote_nsignaled += 1 + additional_signal;
        _remote_rq._mutex.unlock();
        _control->signal_task(1 + additional_signal);
    }
}
```
先给当前TG的 remote_rq 加互斥锁。然后对 remote_rq 进行入队操作，这里是一个while循环，只有入队失败就执行flush_nosignal_tasks_remote_locked()然后休眠1ms，然后重新尝试入队。

这里入队失败的唯一原因就是remote_rq 的容量满了。flush_nosignal_tasks_remote_locked()的操作也无非就是发出一个信号，让remote_rq中的任务（TM/bthread)尽快被消费掉。给新的任务入队留出空间。另外flush_nosignal_tasks_remote_locked()内会做解锁操作，所以休眠1ms之后需要重新加锁。

回看**ready_to_run_remote()，在while结束之后。表示新任务已经入队。前面已讲，**nosignal多为false，所以忽略if(nosignal)的部分，关注else的部分。用当前remote_rq中还没有通知的任务个数+1，去做通知操作。也就是调用TaskControl的**signal_task()。其实就是通知其他人来消费。**

```
    // Tell other groups that `n' tasks was just added to caller's runqueue
    void signal_task(int num_task);
```

## TaskControl::signal_task(int num_task)
看代码：
```
   if (num_task <= 0) {
        return;
    }
    // TODO(gejun): Current algorithm does not guarantee enough threads will
    // be created to match caller's requests. But in another side, there's also
    // many useless signalings according to current impl. Capping the concurrency
    // is a good balance between performance and timeliness of scheduling.
    if (num_task > 2) {
        num_task = 2;
    }
```
num_task 小于等于0 则返回，如果大于2，则重置为2。也就是说下面逻辑中num_task的有效值只有1和2。在上方（BRPC作者）的注释中提到，把num_task不超过2，是在性能和调度时间直接的一种平衡。

这句话如何理解呢？其实是这样，如果TC的signal_task()通知的任务个数多，那么队列被消费的也就越快。消费的快本来是好事，但是也有个问题就是我们现在之所以走到signal_task()是因为我们在“生产”bthread任务，也就是说在执行bthread_start_background()（或其他函数）创建新任务。这个函数调用是阻塞的，如果signal_task()通知的任务个数太多，则会导致bthread_start_background()阻塞的时间拉长。所以这里说是找到一种平衡。

```
int start_index = butil::fmix64(pthread_numeric_id()) % PARKING_LOT_NUM;
num_task -= _pl[start_index].signal(1);
```
start_index计算方式和刚才给TG分配PL的相同，主要就是找到了当前TG（worker）所归属的PL。然后调用这个PL的成员函数signal(1)进行通知。好了，先暂停“生产者”函数调用视角。看下PL的定义，以及其signal()函数。

## ParkingLot 的基础定义
```
class BAIDU_CACHELINE_ALIGNMENT ParkingLot {
public:
    class State {
    public:
        State(): val(0) {}
        bool stopped() const { return val & 1; }
    private:
    friend class ParkingLot;
        State(int val) : val(val) {}
        int val;
    };

    ParkingLot() : _pending_signal(0) {}

    ... 成员函数：signal(int)、get_state()、wait()、stop()

private:
    // higher 31 bits for signalling, LSB for stopping.
    butil::atomic<int> _pending_signal;
};
```
有一个内部类State，其构造函数可以接收一个int。PL是它的友元，另外PL有一个私有成员_pending_signal，是一个原子类型。初始为0。

接着我们看下PL的成员函数signal(int)，也就是前面调用的那个。

```
    // Wake up at most `num_task' workers.
    // Returns #workers woken up.
    int signal(int num_task) {
        _pending_signal.fetch_add((num_task << 1), butil::memory_order_release);
        return futex_wake_private(&_pending_signal, num_task);
    }
```
注释有言：唤醒最多num_task个worker，返回唤醒的worker。

代码实现中，寥寥两行。先给_pending_signal 加上num_task <<1（即num_task*2）。这里之所以累加的数字，要经过左移操作，其目的只是为了让其成为偶数。为什么这里需要一个偶数呢？在文章尾部会有讲解，大家稍安勿躁。

接着调用futex_wake_private(&_pending_signal, num_task)。那么问题又来了，futex_wake_private又是何方神圣呢？

## futex_wake_private()
在src/bthread/sys_futex.h中有定义。另外该文件中还有阈值配套的函数futex_wait_private()
```
inline int futex_wake_private(void* addr1, int nwake) {
    return syscall(SYS_futex, addr1, (FUTEX_WAKE | FUTEX_PRIVATE_FLAG),
                   nwake, NULL, NULL, 0);
}
```
其实就是对于系统调用SYS_futex的封装。这里之所以通过syscall()传参，而不是直接调用的方式，来调用它。是因为SYS_futex没有被glibc export成库函数。我们通常使用的fork()、open()、write()等函数虽然也被称为系统调用，但其实是glibc把系统调用给export出来的封装函数。这个中细节如果想了解，可以参考这个回答的第一部分：

继续介绍一下SYS_futex调用。就是通常说的futex，它是一种用户态和内核态混合的同步机制，可以简单理解为是一种效率较高的同步机制。pthread的很多API大多基于futex实现，细节不再展开。futex系统调用的API声明如下：

```
       int futex(int *uaddr, int op, int val, const struct timespec *timeout,
                 int *uaddr2, int val3);
```
参数解析：
1. uaddr指针指向一个整型，存储一个整数。
2. op表示要执行的操作类型，比如唤醒(FUTEX_WAKE)、等待(FUTEX_WAIT)
3. val表示一个值，注意：对于不同的op类型，val语义不同。
    1. 对于等待操作：如果uaddr存储的整型与val相同则继续休眠等待。等待时间就是timeout参数。
    2. 对于唤醒操作：val表示，最多唤醒val 个阻塞等待uaddr上的“消费者”（之前对同一个uaddr调用过FUTEX_WAIT，姑且称之为消费者，其实在brpc语境中，就是阻塞的worker）。
4. timeout表示超时时间，仅对op类型为等待时有用。就是休眠等待的最长时间。在brpc中uaddr2和val3可以忽略。

返回值解析：
1. 对于等待操作：成功返回0，失败返回-1；
2. 对于唤醒操作：成功返回唤醒的之前阻塞在futex上的“消费者”个数，失败返回-1。

所以futex_wake_private()里面的syscall()等价于：
```
futex(&_pending_signal, (FUTEX_WAKE|FUTEX_PRIVATE_FLAG), num_task, NULL, NULL, 0);
```
FUTEX_WAKE是唤醒操作，FUTEX_PRIVATE_FLAG是一个标记，表示不和其他进程共享，可以减少开销。由于是唤醒操作，在brpc语境下，其返回值就是阻塞的worker个数。它的返回值会一路透传给futex_wake_private()以及PL的signal()函数。

彼时我们的观察视角也可以开始回溯，回到TC的signal_task()了。

继续 **TaskControl::signal_task(int num_task)**

```
int start_index = butil::fmix64(pthread_numeric_id()) % PARKING_LOT_NUM;
num_task -= _pl[start_index].signal(1)
```
_pl[start_index].signal(1)的返回值就是返回的worker个数了。然后将num_task减去唤醒的个数就是需要唤醒，但未唤醒的任务个数。接着看：
```
    if (num_task > 0) {
        for (int i = 1; i < PARKING_LOT_NUM && num_task > 0; ++i) {
            if (++start_index >= PARKING_LOT_NUM) {
                start_index = 0;
            }
            num_task -= _pl[start_index].signal(1);
        }
    }
```
如果num_task不为0，则继续遍历TC的下一个PL，开始执行signal()操作去唤醒阻塞的worker。

接着：
```
    if (num_task > 0 &&
        FLAGS_bthread_min_concurrency > 0 &&    // test min_concurrency for performance
        _concurrency.load(butil::memory_order_relaxed) < FLAGS_bthread_concurrency) {
        // TODO: Reduce this lock
        BAIDU_SCOPED_LOCK(g_task_control_mutex);
        if (_concurrency.load(butil::memory_order_acquire) < FLAGS_bthread_concurrency) {
            add_workers(1);
        }
    }
```
如果任务还有剩余（表示消费者不够用），并且全局TC的并发度（_concurrency）小于gflag中配置的bthread_min_concurrency，那么就调用add_workers()去增加worker的数量。所以FLAGS_bthread_concurrency是**worker(或者说是TG、pthread）个数的硬门槛**。

好了，至此从“生产”bthread任务的角度，已经串完了整个流程。再从消费者的角度看一下ParkingLot。

其实上一篇文章已经对“消费”bthread任务的流程，讲的比较多了，其中涉及到了工作窃取（work stealing）以及汇编语言完成的栈空间切换。但是其中涉及到pl的部分没有终点介绍，我们来回顾一下TG的wait_task()函数。该函数是用来等待任务出现的。

```
bool TaskGroup::wait_task(bthread_t* tid) {
    do {
        if (_last_pl_state.stopped()) {
            return false;
        }
        _pl->wait(_last_pl_state);
        if (steal_task(tid)) {
            return true;
        }
    } while (true);
}
```
_last_pl_state是ParkingLot::State，是TG的一个成员。回看其定义：
```
    class State {
    public:
        State(): val(0) {}
        bool stopped() const { return val & 1; }
    private:
    friend class ParkingLot;
        State(int val) : val(val) {}
        int val;
    };
```
TG初始化的时候_last_pl_state是无参数构造的，所以其val是0。

看下它的stopped()，其实就是判断val是否是奇数！由于我们生产任务时，调用pl的signal()总是累加一个偶数(num_task <<1)：
```
_pending_signal.fetch_add((num_task << 1), butil::memory_order_release);
```
所以TaskGroup::wait_task()中第一个if。if(_last_pl_state.stopped()) 在正常情况下都是不成立的！
不会触发return。而是继续向下走到了：

```
//TaskGroup::wait_task中
        ...
        _pl->wait(_last_pl_state);
```
去等待任务出现。这个wait()在ParkingLot类中定义如下：
```
   // Wait for tasks.
    // If the `expected_state' does not match, wait() may finish directly.
    void wait(const State& expected_state) {
        futex_wait_private(&_pending_signal, expected_state.val, NULL);
    }
```

和生产流程中我们看到的wake()类似，这里的其对等操作wait()，封装的是futex_wait_private()。闲言少叙，其最终等价于：

```
futex(&_pending_signal, (FUTEX_WAIT|FUTEX_PRIVATE_FLAG), expected_state.val, NULL, NULL, 0);
```
关于futex的等待操作，在介绍唤醒操作时也已经提及。这里结合参数可以这样理解，它阻塞在&_pending_signal这里，因为expected_state实际传入的是_last_pl_state，所以该wait操作其预期值也便是_last_pl_state.val。如果&_pending_signal存储的值和_last_pl_state.val相同则阻塞（也就是说还没有任务出现），否则解除阻塞。走到：
```
//TaskGroup::wait_task中
        ...
        if (steal_task(tid)) {
            return true;
        }
```
去调用TG的steal_task()找任务。定义如下

(忽略宏BTHREAD_DONT_SAVE_PARKING_STATE）
```
    bool steal_task(bthread_t* tid) {
        if (_remote_rq.pop(tid)) {
            return true;
        }
        _last_pl_state = _pl->get_state();
        return _control->steal_task(tid, &_steal_seed, _steal_offset);
    }
```
在当前TG的_remote_rq无任务的时候，_last_pl_state会从pl同步一次状态。

PL中的get_state()定义如下：
```
    // Get a state for later wait().
    State get_state() {
        return _pending_signal.load(butil::memory_order_acquire);
    }
```

所以_last_pl_state同步的就是_pending_signal的最新值。其实从last_pl_state的名字早就可以看出，它存储的是上一次pl的状态了！

值得一提的是：&_pending_signal中存储的值其实并不表示任务的个数，尽管来任务来临时，它会做一次加法，但加的并不是任务数，并且在任务被消费后不会做减法。这里面值是没有具体意义的，其变化仅仅是一种状态“同步”的媒介！就像小说和电影中的工具人！。



好了，前面说了_last_pl_state正常情况下，判断stopped()都是不成立的，那么什么时候会成立呢？还是在ParkingLot中，它有一个stop()成员函数：
```
    // Wakeup suspended wait() and make them unwaitable ever. 
    void stop() {
        _pending_signal.fetch_or(1);
        futex_wake_private(&_pending_signal, 10000);
    }
```
其中会做fetch_or(1)操作，经此一役，_last_pl_state必然为奇数。而调用pl的stop()函数的地方只有一处，那就是TC中的stop_and_join()，而stop_and_join()又只在bthread_stop_world()这个函数调用的中被调用。调用链如下：

* bthread_stop_world()
    * TaskControl::stop_and_join()
        * ParkingLot::stop()

正常我们都不会调用，bthread_stop_world()，所以在_last_pl_state.stopped()在服务正常运转的情况下都不会为false。


# bthread上下文的创建
在之前的文章有介绍过bthread上下文的切换（jump_stack，bthread栈的切换)，其中涉及了汇编语言。本文来讲一讲与之对应的另外一个操作：上下文的创建（get_stack()，bthread栈的创建）。

其实涉及到上下文创建的有两处，一处是TaskGroup初始化的时候，另外一个就是TaskGroup在死循环获取任务执行任务的时候，在jump_stack()之前会调用get_stack()。

先看一下TaskGroup的初始化。
## TaskControl::create_group()
```
TaskGroup* TaskControl::create_group() {
    TaskGroup* g = new (std::nothrow) TaskGroup(this);
    if (NULL == g) {
        LOG(FATAL) << "Fail to new TaskGroup";
        return NULL;
    }
    if (g->init(FLAGS_task_group_runqueue_capacity) != 0) {
        LOG(ERROR) << "Fail to init TaskGroup";
        delete g;
        return NULL;
    }
    if (_add_group(g) != 0) {
        delete g;
        return NULL;
    }
    return g;
}
```
在TaskGroup初始化的时候，会创建TaskGroup，并调用TaskGroup::init()初始化。

## TaskGroup::init()
```
int TaskGroup::init(size_t runqueue_capacity) {
    if (_rq.init(runqueue_capacity) != 0) {
        LOG(FATAL) << "Fail to init _rq";
        return -1;
    }
    if (_remote_rq.init(runqueue_capacity / 2) != 0) {
        LOG(FATAL) << "Fail to init _remote_rq";
        return -1;
    }
    ContextualStack* stk = get_stack(STACK_TYPE_MAIN, NULL);
   ...
```
可以看出gflag变量 **FLAGS_task_group_runqueue_capacity** 控制着TG中rq和remote_rq队列的容量（默认是4096），如果你想扩大TG中两个任务队列的大小，请修改task_group_runqueue_capacity这个gflags。当前这是题外话。

重点关注一下get_stack()。初始化的时候调用getstack()，第二个参数是NULL。这便是get_stack()第一类调用的地方。另外一处是在TaskGroup::ending_sched()中。

## TaskGroup::ending_sched()
```
  TaskMeta* const cur_meta = g->_cur_meta;
    TaskMeta* next_meta = address_meta(next_tid);
    if (next_meta->stack == NULL) {
        if (next_meta->stack_type() == cur_meta->stack_type()) {
            next_meta->set_stack(cur_meta->release_stack());
        } else {
            ContextualStack* stk = get_stack(next_meta->stack_type(), task_runner);
            if (stk) {
                next_meta->set_stack(stk);
            } else {
                next_meta->attr.stack_type = BTHREAD_STACKTYPE_PTHREAD;
                next_meta->set_stack(g->_main_stack);
            }
        }
    }
    sched_to(pg, next_meta);
```
这里也会调用get_stack()，其第二个参数是task_runner而不是NULL了。这里会获取一个表示栈结构的stk，赋值给next_meta。在最后的sched_to()中会调用之前介绍过的jump_stack()

## get_stack()
src/bthread/stack_inl.h中
```
inline ContextualStack* get_stack(StackType type, void (*entry)(intptr_t)) {
    switch (type) {
    case STACK_TYPE_PTHREAD:
        return NULL;
    case STACK_TYPE_SMALL:
        return StackFactory<SmallStackClass>::get_stack(entry);
    case STACK_TYPE_NORMAL:
        return StackFactory<NormalStackClass>::get_stack(entry);
    case STACK_TYPE_LARGE:
        return StackFactory<LargeStackClass>::get_stack(entry);
    case STACK_TYPE_MAIN:
        return StackFactory<MainStackClass>::get_stack(entry);
    }
    return NULL;
}
```

根据栈类型的不同，调用不同的工厂函数去做实际的get_stack()操作。这里合法的栈类型公用4种，分别是：
1. SmallStackClass
2. NormalStackClass
3. LargeStackClass
4. MainStackClass

而这4中类型又需要分成两类，MainStackClass自成一类，其余三个为一类。为什么这么说呢？
因为SmallStackClass、NormalStackClass、LargeStackClass用到是StackFactory的通用模板：**template<typename StackClass> struct StackFactory** 而MainStackClass用到的是特化模板: **template <> struct StackFactory<MainStackClass>**

## StackFactory通用模板

先看一下StackFactory的通用模板定义：
```
template <typename StackClass> struct StackFactory {

    struct Wrapper : public ContextualStack {
        explicit Wrapper(void (*entry)(intptr_t)) {
            if (allocate_stack_storage(&storage, *StackClass::stack_size_flag,
                                       FLAGS_guard_page_size) != 0) {
                storage.zeroize();
                context = NULL;
                return;
            }
            context = bthread_make_fcontext(storage.bottom, storage.stacksize, entry);
            stacktype = (StackType)StackClass::stacktype;
        }
        ~Wrapper() {
            if (context) {
                context = NULL;
                deallocate_stack_storage(&storage);
                storage.zeroize();
            }
        }
    }; // end of struct Wrapper
    
    static ContextualStack* get_stack(void (*entry)(intptr_t)) {
        return butil::get_object<Wrapper>(entry);
    }
    
    static void return_stack(ContextualStack* sc) {
        butil::return_object(static_cast<Wrapper*>(sc));
    }
};
```

它包含两个成员函数，一是获取栈（get_statck），另外一个是归还栈（return_stack）。所谓的获取栈就是创建ContextualStack（子类）对象，然后做了初始化。“归还栈”则是“获取栈”的逆操作。

另外StackFactory模板中有一内部类Wrapper，它是ContextualStack的子类。StackFactory成员函数get_stack()和return_stack()操作的其实就是Wrapper类型。

Wrapper的构造函数接收一个参数entry，entry的类型是一个函数指针。void(*entry)(intptr_t)表示的是参数类型为intptr_t，返回值为void的函数指针。intptr_t 是和一个机器相关的整数类型，在64位机器上对应的是long，在32位机器上对应的是int。

其实entry只有两个值，一种是NULL，另外一个就是 TaskGroup中的static函数：task_runner()。

```
    static void task_runner(intptr_t skip_remained);
```

构造函数内会调用allocate_stack_storage()分配栈空间，接着对storage、context、stacktype的初始化。这三个是父类ContextualStack的成员。

其中context的初始化会调用bthread_make_fcontext()函数。还记得在前面文章中解读过的bthread_jump_fcontext()吗？没错，这个就是和他一起定义的另外一个汇编语言实现的函数。这里先按下不表。

Wrapper析构的时候会调用deallocate_stack_storage()释放占空间，并重置三个成员变量。

## StackFactory<MainStackClass> 特化模板
再看一下MainStackClass的特化模板：
```
template <> struct StackFactory<MainStackClass> {
    static ContextualStack* get_stack(void (*)(intptr_t)) {
        ContextualStack* s = new (std::nothrow) ContextualStack;
        if (NULL == s) {
            return NULL;
        }
        s->context = NULL;
        s->stacktype = STACK_TYPE_MAIN;
        s->storage.zeroize();
        return s;
    }
    
    static void return_stack(ContextualStack* s) {
        delete s;
    }
};
```
比较简洁，最大的区别就是它没有Wrapper，没有调用bthread_make_fcontext()，也就是没有分配上下文。

## ContextualStack类型
好了，我们看下ContextualStack定义：
```
struct ContextualStack {
    bthread_fcontext_t context;
    StackType stacktype;
    StackStorage storage;
};
```

bthread_fcontext_t其实是void*的别名。

StackType是栈类型的枚举，所以 stacktype用来记录栈的类型。

```
enum StackType {
    STACK_TYPE_MAIN = 0,
    STACK_TYPE_PTHREAD = BTHREAD_STACKTYPE_PTHREAD,
    STACK_TYPE_SMALL = BTHREAD_STACKTYPE_SMALL,
    STACK_TYPE_NORMAL = BTHREAD_STACKTYPE_NORMAL,
    STACK_TYPE_LARGE = BTHREAD_STACKTYPE_LARGE
};
```
StackStorage是具体表示栈信息的：
```
struct StackStorage {
     int stacksize;
     int guardsize;
    // Assume stack grows upwards.
    // http://www.boost.org/doc/libs/1_55_0/libs/context/doc/html/context/stack.html
    void* bottom;
    unsigned valgrind_stack_id;

    // Clears all members.
    void zeroize() {
        stacksize = 0;
        guardsize = 0;
        bottom = NULL;
        valgrind_stack_id = 0;
    }
};
```
视线上移，重回StackFactory的通用模板，在Warpper的构造函数中有调用allocate_stack_storage()分配栈存储。我们看下：

## allocate_stack_storage()
三种使用通用模板的栈类型，其主要差异就在于分配的栈大小不同了。

allocate_stack_storage函数声明如下：
```
// Allocate a piece of stack.
int allocate_stack_storage(StackStorage* s, int stacksize, int guardsize);
```
第一个参数是表示存储的指针s，表示栈大小的stacksize，表示保护页大小的guardsize。

先看下它是如何被调用的：
```
if (allocate_stack_storage(&storage, *StackClass::stack_size_flag,
                                       FLAGS_guard_page_size) != 0) {
...
}
```

保护页的大小guardsize是通过gflag定义的，对应FLAGS_guard_page_size 其默认值是4096。

栈大小stacksize也就对应的三种栈类型中的stack_size_flag，也都是通过gflag定义：

```
int* SmallStackClass::stack_size_flag = &FLAGS_stack_size_small;  // 默认值32768
int* NormalStackClass::stack_size_flag = &FLAGS_stack_size_normal;// 默认值1048576
int* LargeStackClass::stack_size_flag = &FLAGS_stack_size_large;  // 默认值8388608
```

> 可以思考一下：为什么stack_size_flag要定义成int*指针类型，而不是直接定义成int类型？

开始看allocate_stack_storage()的实现，它的定义代码很长，我们分段来看。
```
int allocate_stack_storage(StackStorage* s, int stacksize_in, int guardsize_in) {
    
    const static int PAGESIZE = getpagesize();
    const int PAGESIZE_M1 = PAGESIZE - 1;
    const int MIN_STACKSIZE = PAGESIZE * 2;
    const int MIN_GUARDSIZE = PAGESIZE;
```
在源文件定义中，参数二三的名称有调整，换成了stacksize_in和guardsize_in。它们就是刚才我们说的stacksize和guardsize（之所以改了个名字是因为下面还有变量会用到stacksize和guardsize这两个名字）。

getpagesize()是<unistd.h>中的库函数，用来获取系统的一个分页的大小（所在内存的字节数）。上面共定义了4个页大小相关的变量。
```
    // Align stacksize
    const int stacksize =
        (std::max(stacksize_in, MIN_STACKSIZE) + PAGESIZE_M1) &
        ~PAGESIZE_M1;
```
这里涉及到二进制运算，其实就是让内存大小按照页大小对齐（也就是页大小的整数倍）。可能理解计算过程会比较绕，不过我直接说一下结论就好。比如在我的Linux和Mac上页大小都是4096，然后经过上述运算stacksize的值基本上都是和传入的stacksize_in相同！这是因为三种栈的大小已经是4096的整数倍了。好了，不用纠结，我们继续。
```
if (guardsize_in <= 0) {
    ...
    ...
    ...
} else {
```
因为我们的guardsize_in 默认是4096的（一般也没人去改它），我们直接忽略这个if里面的代码，直接看else。
```
        // Align guardsize
        const int guardsize =
            (std::max(guardsize_in, MIN_GUARDSIZE) + PAGESIZE_M1) &
            ~PAGESIZE_M1;
```
和前面一样的计算过程，进行对齐。在我的Linux上计算之后的guardsize就是4096，等同于guardsize_in。这个毋庸置疑。
```
        const int memsize = stacksize + guardsize;
        void* const mem = mmap(NULL, memsize, (PROT_READ | PROT_WRITE),
                               (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
        if (MAP_FAILED == mem) {
            PLOG_EVERY_SECOND(ERROR) 
                << "Fail to mmap size=" << memsize << " stack_count="
                << s_stack_count.load(butil::memory_order_relaxed)
                << ", possibly limited by /proc/sys/vm/max_map_count";
            // may fail due to limit of max_map_count (65536 in default)
            return -1;
        }
```
用mmap分配一块内存，大小是stacksize，guardsize之和。
```
        void* aligned_mem = (void*)(((intptr_t)mem + PAGESIZE_M1) & ~PAGESIZE_M1);
        if (aligned_mem != mem) {
            LOG_ONCE(ERROR) << "addr=" << mem << " returned by mmap is not "
                "aligned by pagesize=" << PAGESIZE;
        }
```
这个是判断一下mmap返回的内存地址是不是按照页大小对齐的。如果不是就打一行ERROR日志。
```
     const int offset = (char*)aligned_mem - (char*)mem;
        if (guardsize <= offset ||
            mprotect(aligned_mem, guardsize - offset, PROT_NONE) != 0) {
            munmap(mem, memsize);
            PLOG_EVERY_SECOND(ERROR) 
                << "Fail to mprotect " << (void*)aligned_mem << " length="
                << guardsize - offset; 
            return -1;
        }
```
计算offset，当不对齐的时候offset会大于0。接着如果offset大于保护页的大小，直接返回-1。如果offset小于保护页的大小，就调用mprotect()把多余的字节（guardsize - offset）设置成不可访问（PROT_NONE）。
```
   s_stack_count.fetch_add(1, butil::memory_order_relaxed);
```
全局原子变量s_stack_count 加1。
```
        s->bottom = (char*)mem + memsize;
        s->stacksize = stacksize;
        s->guardsize = guardsize;
```
给allocate_stack_storage()第一个参数s的三个字段赋值。

s->bottom存储的是栈底部的地址，因为mem是开始的地址，memsize是长度，二者相加就到尾部了。
```
    if (RunningOnValgrind()) {
            s->valgrind_stack_id = VALGRIND_STACK_REGISTER(
                s->bottom, (char*)s->bottom - stacksize);
        } else {
            s->valgrind_stack_id = 0;
        }
```
如果当前是在运行Valgrind（检查内存泄漏的工具）则执行一些逻辑。这个是调试和分析时用的，可以忽略这段逻辑。

接下来我们重新回到get_stack()这个函数上来，在StackFactory中：
```
    static ContextualStack* get_stack(void (*entry)(intptr_t)) {
        return butil::get_object<Wrapper>(entry);
    }
```
## butil::get_object()
butil::get_object()是brpc实现的对象池相关函数。定义在butil/object_pool_inl.h 中，**get_object()是一个模板函数，有三个重载，分别支持构造函数为0个参数、1个参数、2个参数的类对象。**

在我们这里的场景中，用到的是1个参数重载：
```
    template <typename A1>
    inline T* get_object(const A1& arg1) {
        LocalPool* lp = get_or_new_local_pool();
        if (BAIDU_LIKELY(lp != NULL)) {
            return lp->get(arg1);
        }
        return NULL;
    }
```
BAIDU_LIKELY是一个宏，直接展开：
```
    template <typename A1>
    inline T* get_object(const A1& arg1) {
        LocalPool* lp = get_or_new_local_pool();
        if (__builtin_expect((bool)(lp != __null), true)) {
            return lp->get(arg1);
        }
        return NULL;
    }
```
get_or_new_local_pool()是获取一个段内存区lp（这个是thread local的）

下面的__builtin_expect()是gcc扩展函数，方便编译器做分支预测优化的。这里表示就是lp 大概率都不等于NULL，会比写普通的if (lp != __null)性能更好。但逻辑上是等价的：
```
    if (lp != __null) {
            return lp->get(arg1);
        }
```
看下lp->get(arg1)的实现（还是butil/object_pool_inl.h 中）。这个get()也是有三个重载，分别支持0个参数，1个参数和2个参数。
```
 template <typename A1>
        inline T* get(const A1& a1) {
            BAIDU_OBJECT_POOL_GET((a1));
        }
```
**BAIDU_OBJECT_POOL_GET是一个复杂的宏**。这个就是所谓对象池的主要逻辑了，我这里直接展开，然后添加一些注释。
```
// 如果对象池中有剩余，则直接返回
if (_cur_free.nfree) {
    BAIDU_OBJECT_POOL_FREE_ITEM_NUM_SUB1;
    return _cur_free.ptrs[--_cur_free.nfree];
}
// 对象池中无剩余，TODO
if (_pool->pop_free_chunk(_cur_free)) {
    BAIDU_OBJECT_POOL_FREE_ITEM_NUM_SUB1;
    return _cur_free.ptrs[--_cur_free.nfree];
}
// 使用定位new，在指定内存位置去构造对象。
// 在我们这个场景中就是构造Wrapper对象，a1就是传入的函数指针
// 如果成功则直接把构造好的对象指针返回
if (_cur_block && _cur_block->nitem < BLOCK_NITEM) {
    T *obj = new ((T *)_cur_block->items + _cur_block->nitem) T(a1);
    if (!ObjectPoolValidator<T>::validate(obj)) {
        obj->~T();
        return NULL;
    }
    ++_cur_block->nitem;
    return obj;
}
// 走到这说明构造对象失败了，则新建一个block
// 还是用定位new，在指定位置构造对象
_cur_block = add_block(&_cur_block_index);
if (_cur_block != NULL) {
    T *obj = new ((T *)_cur_block->items + _cur_block->nitem) T(a1);
    if (!ObjectPoolValidator<T>::validate(obj)) {
        obj->~T();
        return NULL;
    }
    ++_cur_block->nitem;
    return obj;
}
return NULL;
```
在上面代码中obj构造完成之后，返回之前。都会做一个if(!ObjectPoolValidator<T>::validate(obj))的验证。顾名思义是去验证一下obj是否是有效的。通用模板恒为true。
```
template <typename T> struct ObjectPoolValidator {
    static bool validate(const T*) { return true; }
};
```
不同的类型可以自己实现特化的模板，比如我们的三种栈类型：
```
template <> struct ObjectPoolValidator<
    bthread::StackFactory<bthread::LargeStackClass>::Wrapper> {
    inline static bool validate(
        const bthread::StackFactory<bthread::LargeStackClass>::Wrapper* w) {
        return w->context != NULL;
    }
};

template <> struct ObjectPoolValidator<
    bthread::StackFactory<bthread::NormalStackClass>::Wrapper> {
    inline static bool validate(
        const bthread::StackFactory<bthread::NormalStackClass>::Wrapper* w) {
        return w->context != NULL;
    }
};

template <> struct ObjectPoolValidator<
    bthread::StackFactory<bthread::SmallStackClass>::Wrapper> {
    inline static bool validate(
        const bthread::StackFactory<bthread::SmallStackClass>::Wrapper* w) {
        return w->context != NULL;
    }
};
```
一定要context不为NULL才是有效的。

至此大部分基本讲完了。还剩一个重点没讲，那就是汇编实现的**bthread_make_fcontext()!**

## bthread_make_fcontext()
先回顾一下它被调用的地方：
```
context = bthread_make_fcontext(storage.bottom, storage.stacksize, entry);
```
bthread_make_fcontext()作用是在当前栈顶创建一个上下文，用来执行第三个参数表示的函数entry。返回ContextualStack*类型上下文 。通过前文我们知道entry只有两种取值，一个是NULL，另外一个就是task_runner。
```
    static void task_runner(intptr_t skip_remained);
```
看下bthread_make_fcontext()的定义吧，src/bthread/context.cpp中
```
#if defined(BTHREAD_CONTEXT_PLATFORM_linux_x86_64) && defined(BTHREAD_CONTEXT_COMPILER_gcc)
__asm (
".text\n"
".globl bthread_make_fcontext\n"
".type bthread_make_fcontext,@function\n"
".align 16\n"
"bthread_make_fcontext:\n"
"    movq  %rdi, %rax\n"
"    andq  $-16, %rax\n"
"    leaq  -0x48(%rax), %rax\n"
"    movq  %rdx, 0x38(%rax)\n"
"    stmxcsr  (%rax)\n"
"    fnstcw   0x4(%rax)\n"
"    leaq  finish(%rip), %rcx\n"
"    movq  %rcx, 0x40(%rax)\n"
"    ret \n"
"finish:\n"
"    xorq  %rdi, %rdi\n"
"    call  _exit@PLT\n"
"    hlt\n"
".size bthread_make_fcontext,.-bthread_make_fcontext\n"
".section .note.GNU-stack,\"\",%progbits\n"
);

#endif
```
bthread_make_fcontext()逻辑没有bthread_jump_fcontext()复杂。

逐步来看汇编代码。
```
movq  %rdi, %rax
```
%rdi存储的是 第一个参数(也就是storage.bottom)复制到%rax寄存器中。
```
andq  $-16, %rax
```
%rax 存储的值减去16，表示对齐。设第一个参数为n（也就是storage.bottom)，则这个命令表示 %rax=(8n+22)&-16 求得storage.bottom向下舍入16的最小的倍数，当n为奇数的时候为8n+8；当n为偶数的时候为8n+16; %rax 是用法作为返回值的，这里也就是通过storage.bottom计算出一个实际要返回的栈地址（不是直接返回storage.bottom）
```
leaq  -0x48(%rax), %rax
```
%rax存储地址减去72，再存入%rax寄存器中。
```
movq  %rdx, 0x38(%rax)
```
%rdx存储的是第三个参数（也就是函数指针变量entry）存入%rax指向地址+56的位置。
```
stmxcsr  (%rax)
fnstcw   0x4(%rax)
```
保存MXCSR寄存器的值到%rax指向地址，保存当前FPU状态字到%rax+4的地址。（bthread_jump_fcontext 中也有类似操作）
```
leaq  finish(%rip), %rcx
```
计算finish标签的地址，存入%rcx。
```
movq  %rcx, 0x40(%rax)
```
把%rcx的值存入%rax+64指向的地址。
```
finish:
    xorq  %rdi, %rdi
    call  _exit@PLT
    hlt

```
xorq就是异或操作， xorq %rdi,%rdi 就是把%rdi寄存器清零。

后面两句是退出和暂停。

[Swoole协程之旅-后篇-Swoole 官方文档手册-面试哥](http://www.mianshigee.com/tutorial/SwooleDoc/158.md/)

[学习笔记 变长栈帧_qq_40065223的博客-CSDN博客](https://blog.csdn.net/qq_40065223/article/details/77948824)

### 1. bthread_make_fcontext()虽然传入了栈大小，但是却没有被用到？
```
context = bthread_make_fcontext(storage.bottom, storage.stacksize, entry);
```
bthread_make_fcontext()最终会调用汇编函数make_stack()，其中用到了第一个参数（栈底）,却没有用到栈大小。那么你是否有疑问，如果没有用到栈大小，那么是不是前面给bthread设置栈大小的属性参数就没有用呢？

非也非也。这里虽然汇编函数内确实没有用到栈的大小，但是storage.bottom其实已经是bthread可用的栈的最高地址了。回顾bottom的分配策略我们可知：

```
        s->bottom = (char*)mem + memsize;
```

分配的栈大小确实是和bthread设置栈大小的属性参数有关的，然后这里返回的bottom是最高地址。也就是说时间在栈切换过去执行具体的协程任务的时候，由于栈是自高向低分配的，所以只要你使用的栈空间不超过大小就是OK的。在新局部变量分配的时候并没有进行边界检查，这在C语言里面亦是如是。当你使用的栈空间超过的时候，这里确实也会越界。

我曾经遇到过使用一个某个第三方库，会导致程序会core掉，归根结底是因为我设置的的栈大小是SMALL的，而这个库用到的栈空间比较多，改成NORMAL之后core解决。

### 2. 为什么要用汇编做任务切换，让worker（pthread）不停的从任务队列里取出任务去执行不就好了么？
看完前面的文章你可能会疑惑，Task已经封装到任务队列里了（rq，remote_rq)，直接遍历队列取出任务，执行其中的回调函数就OK啦。干嘛要用汇编做一套make_stack和jump_stack呢？

我可以理解你的想法，在没有协程概念之前，很多线程池的涉及都如你这般，包括work stealing也可以被普通的线程池采纳。但是这里你永远要记住bthread的协程属性。所谓协程就是可以被中断的，也就是说你的bthread回调函数可以执行一半就退出worker，然后让给其他的bthread任务，等后续再唤醒回来继续执行刚才的运行了一半的任务。

这里的中断与否，其实就是看你的回调函数中是否有涉及到IO。比如brpc的Channel操作。如果你在通过RPC访问其他的brpc服务或者HTTP服务或者Redis等，这些操作都会让你的bthread退出，解除对worker的占用。待到下游response返回之后，才继续找到一个闲置的worker来处理你运行了一半的任务。这个中细节其实也颇为复杂，后续我会开篇去讲Channel的CallMethod与bthread的关联。

### 3. bthread适合做什么与不适合做什么
通过bthread，我们可以很方便的实现一个并行的调度库，我在工作中确实有见到过基于bthread实现的并行调度库，比如实现一个DAG在线调度引擎。借助于强大的bthread，我们只需要对于bthread_start_background()调用和封装一下就能实现。最复杂的部分brpc已经帮我们做了。

但诚然如此，我们还是要知道，bthread主要还是在充分利用的网络IO时的计算资源，通过M:N的协程来实现CPU利用的最大化。如果你的程序不是IO密集型的，只是单纯的计算密集型的，你当然也可以通过bthread来做并行化（比如for循环顺序处理无10000个元素，可以做出并行化的），但你需要知道这种场景使用bthread并不会比那些老牌的并行计算库更有优势，比如OpenMP、TBB等。


