## 调度执行bthread的主要数据结构

在一个线上环境系统中，会产生大量的bthread，系统的cpu核有限，如何让大量的bthread在有限的cpu核心上得到充分调度执行，实现全局的最大并发主要是由TaskGroup对象、TaskControl对象实现的。

1. 每一个TaskGroup对象是系统线程pthread的线程私有对象，它内部包含有任务队列，并控制pthread如何执行任务队列中的众多bthread任务。TaskGroup中主要的成员有：
   - _remote_rq: 如果一个pthread 1想让pthread 2执行bthread 1，则pthread 1会将bthread 1的tid压入pthread 2的TaskGroup的_remote_rq队列中。
   - _rq: pthread 1在执行从自己私有的TaskGroup中取出bthread 1时，如果bthread 1执行过程中又创建了新的bthread 2，则bthread 1将bthread 2的tid压入pthread 1的TaskGroup的_rq队列中。
   - _main_tid & _main_stack：一个pthread会在TaskGroup::run_main_task()中执行while()循环，不断获取并执行bthread任务，一个pthread的执行流不是永远在bthread中，比如等待任务时，pthread没有执行任何bthread，执行流就是直接在pthread上。可以将pthread在『等待bthread-获取到bthread-进入bthread执行任务函数之前』这个过程也抽象成一个bthread，称作一个pthread的『调度bhtread』或者『主bthread』，它的tid和私有栈就是_main_tid和_main_stack。
   - _cur_meta: 当前正在执行的bthread的TaskMeta对象的地址。

2. TaskControl对象是全局的单例对象，主要成员有：
   - _pl: ParkingLot类型的数组。ParkingLot对象用于bthread任务的等待-通知。
   - _workers: pthread线程标致符的数组，表示创建了多少个pthread worker线程，每个pthread worker线程应拥有一个线程私有的TaskGroup对象。
   - _groups: TaskGroup对象指针的数组。