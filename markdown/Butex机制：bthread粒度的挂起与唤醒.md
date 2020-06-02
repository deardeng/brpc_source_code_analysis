## bthread粒度挂起与唤醒的设计原理
由于bthread任务是在pthread系统线程中执行，在需要bthread间互斥的场景下不能使用pthread级别的锁（如pthread_mutex_lock或者C++的unique_lock等），否则pthread会被挂起，不仅当前的bthread中止执行，pthread私有的TaskGroup的任务队列中其他bthread也无法在该pthread上调度执行。因此需要在应用层实现bthread粒度的互斥机制，一个bthread被挂起时，pthread任然要保持运行状态，保证TaskGroup任务队列中的其他bthread的正常执行不受影响。
要实现bthread粒度的互斥，方案如下：
1. 在同一个pthread上执行的多个bthread是串行执行的，不需要考虑互斥；
2. 如果位于heap内存上或static静态区上的一个对象A可能会被在不同pthread执行的多个bthread同时访问，则为对象A维护一个互斥锁（一般是一个原子变量）和等待队列，同时访问对象A的多个bthread首先要竞争锁，假设三个bthread 1、2、3分别在pthread 1、2、3上执行，bthread 1、bthread 2、bthread 3同时访问heap内存上的一个对象A，这时就产生了竞态，假设bthread 1获取到锁，可以去访问对象A，bthread 2、bthread 3先将自身必要的信息（bthread的tid等）存入等待队列，然后自动yiled，让出cpu，让pthread2、pthread3继续去执行各自私有TaskGroup的任务队列中的下一个bthread，这就实现了bthread粒度的挂起；
3. bthread 1访问完对象A后，通过查询对象A的互斥锁的等待队列，能够得知bthread 2、bthread 3因等待锁而被挂起，bthread 1负责将bthread 2、3的tid重新压入某个pthread（不一定是之前执行bthread2、3的pthread2、3）的TaskGroup的任务队列，bthread2、3就能够再次被pthread执行，这就实现了bthread粒度的唤醒。

下面分析brpc是如何实现bthread粒度的挂起与唤醒的。

## brpc中Butex的源码解释
brpc实现bthread互斥的主要代码在src/bthread/butex.cpp中：
1. 首先解释下Butex、ButexBthreadWaiter等主要的数据结构：
   ```cpp
   struct BAIDU_CACHELINE_ALIGNMENT Butex{
       Butex(){}
       ~Butex(){}

       // 锁变量的值。
       butil::atomic<int> value;
       // 等待队列，存储等待互斥锁的各个bthread的信息。
       ButexWaiterList waiters;
       internal::FastPthreadMutex waiter_lock;
   }
   ```
   ```cpp
   // 等待队列实际上是个侵入式双向链表，增减元素的操作都可在O（1）时间内完成。
   typedef butil::LinkedList<ButexWaiter> ButexWaiterList;
   ```
   ```cpp
   // ButexWaiter是LinkNode的子类，LinkNode里只定义了指向前后节点的指针。
   struct ButexWaiter : public butil::LinkNode<ButexWaiter>{
       // tids of pthreads are 0
       // tid就是64位的bthread id。
       // Butex实现了bthread间的挂起&唤醒，也实现了bthread和pthread间的挂起&唤醒，
       // 一个pthread在需要的时候可以挂起，等待适当的时候被一个bthread唤醒，线程挂起不需要tid，填0即可。
       // todo:
       // pthread被bthread唤醒的例子可参考example目录下的一些client.cpp示例程序，执行main函数的pthread
       // 会被挂起，某个bthread执行完自己的任务后会去唤醒pthread。
       bthread_t tid;

       // Erasing node from middle of LinkedList is thread-unsafe, we need
       // to hold its container's lock
       butil::atomic<Butex*> container;
   }
   ```
2. 