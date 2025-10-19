---
title: lab2:物理内存和页表
author: 钱俊玮 朱荟宇 邹博闻
---
# <center>lab2:物理内存和页表</center>

#### <center> 钱俊玮&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;朱荟宇&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;邹博闻 </center>
#### <center> 2312480&nbsp;2311824&nbsp;2312251 </center>
### 练习1：理解first-fit 连续物理内存分配算法

#### 算法目的与总体思路

物理页分配器负责管理系统中所有可用的物理页帧。它必须能在运行时快速地：
+ 找到一段满足需求的连续空闲页；
+ 在释放时把相邻的空闲段合并，避免碎片化。

uCore 的默认分配器选择了最简单、开销最低的策略——first-fit。
它维护一条按照物理地址排序的空闲块链表`free_list`。每个节点代表一段连续空闲页，而节点本身对应这段空闲块的块头页。块头页中的`property`字段记录整块的长度（页数），同时该页在`flags`中会被标记为`PageProperty`。

#### 代码分析

我们首先在这里介绍default_pmm.c中各函数的作用，再从整体描述整个程序在物理内存分配的作用。

##### default_init

这是分配器的起点。它清空空闲链表并将`nr_free`（空闲页总数）置 0，相当于建立一个“空目录”。之后所有的物理内存区间都将以空闲块的形式插入到这个链表中。

##### default_init_memmap

这是登记空闲块的过程。它接收一段连续物理页 \[base, base+n)，把这些页的元数据初始化：
+ 每页的引用计数`ref`置 0；
+ 清除`Reserved`位；
+ 仅在块头页`base`上设定`PageProperty=1`并写入`property=n`。

这样 \[base, base+n) 被视为一个独立的空闲块，并按照物理地址顺序插入 free_list。这一步通常在系统启动时由`page_init()`调用，把 QEMU 模拟的整片 DRAM 空间一次性挂到分配器管理下。

##### default_alloc_pages

这是 first-fit 算法的核心。每次请求分配 n 页时，分配器从`free_list`的头开始线性扫描，寻找第一个`property >= n`的空闲块。
+ 如果正好相等，整块取走；
+ 如果更大，就从块头切出 n 页返回，余下部分重新作为一个空闲块登记。
这一步并不会分配“新的 Page 结构体”，而是直接操作之前初始化好的`pages[]`元数据，更新标志与计数。被取走的块头页本身就是可用的物理页。

##### default_free_pages

释放时，函数接收一个块头页指针和块大小 n，把这段页重新标记为空闲块：`property=n`，`PageProperty=1`，并插回 `free_list`。随后它会检查链表前后的块是否与当前块相邻。如果是，就直接合并，更新 `property`，并清除被合并块的标志。通过这种“就地合并”，系统保持空闲链表中不存在两个相邻块。

#### 物理内存分配

结合 `pmm.c`，整个系统的初始化过程大致如下：
1. 启动时，内核通过 $dtb\_init()\to page\_init()$ 探测 QEMU 提供的 DRAM 地址范围；
2. 扣除内核自身和页描述符数组`pages[]`占用的区域；
3. 对剩余物理空间调用 `default_init_memmap()`，登记为一个大的空闲块；
4. 从此刻起，系统所有的 `alloc_pages` / `free_pages` 请求都通过上面的 first-fit 逻辑来分配和回收页帧。

#### 改进

+ 在 `free_list` 基础上引入简单的**分级链表**（例如按块大小分桶）以减少平均查找长度；
+ 或者采用“Next-Fit”记忆上次分配位置，避免每次都从头扫描。

### 练习2：实现 Best-Fit 连续物理内存分配算法

参考kern/mm/default_pmm.c对First Fit算法的实现，编程实现Best Fit页面分配算法，下面将紧密结合编程内容，从三个方面解释目前分配方法的工作流程。

#### 内存初始化

初始化任务主要由best_fit_init_memmap()函数实现，这个函数接收一个Page指针base和整数n，负责初始化base开始的连续n个Page。
##### 遍历设置页属性
Page.property存储该内存块中连续页的数目，这个属性只在块首的页有意义，因此将base页的property设置为n，后续n个页清空property；同时根据hint，此处应该清空引用计数，因为初始化阶段并没有实际用到某一页。

```cpp=
    struct Page *p = base;
    for (; p != base + n; p++)
    {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
```

##### 链入freelist
遍历页并设置好页属性后，将该空闲块首的页链入freelist，并且freelist中空闲页个数增加n，即nr_free+=n。此处有两种情况：若freelist为空，则直接作链表的第一个元素；若不为空，则需要找到合适的插入位置。

由于是一个循环双向链表，因此在从头遍历链表项时，如果到达链表尾部，则继续next会回到链表头。

```cpp=
#define le2page(le, member)                 \
    to_struct((le), struct Page, member)
#define to_struct(ptr, type, member)                               \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```
对链表中每一项通过le2page宏获得page首地址，计算方法为传入一个结构体成员和它对应的地址，因为结构体中成员地址=首地址+结构体内偏移，因此就能推出结构体首地址，即取得page地址。后续是简单的链表插入操作，Lab2框架已经封装好了。

#### 内存分配

best fit分配时的逻辑为在freelist中寻找最接近请求大小的空闲块，以尽可能减小碎片。如果请求页数直接大于freelist中总页数，则分配失败；对可以分配的情况，先对双向链表进行遍历，利用min_size变量找到最接近n且大于n的块。如果确实有这种块，则尝试取出并分配：
```cpp=
    if (page != NULL){
        list_entry_t *prev = list_prev(&(page->page_link));
        list_del(&(page->page_link));// freelist删除该块
        if (page->property > n){// 如果产生内存碎片
            struct Page *p = page + n;
            p->property = page->property - n;// 内存碎片大小
            SetPageProperty(p);
            list_add(prev, &(p->page_link));// 把剩下的内存放回freelist
            // 因为freelist元素是根据页首地址排列的，因此碎片直接放回即可
        }
        nr_free -= n;
        ClearPageProperty(page);
    }
```
#### 内存释放

内存释放主要包括三方面内容：页属性设置，插入到freelist，尝试合并块。

##### 页属性设置
实际上此处与init函数高度相似，也采用遍历页清理flag的方式，只是需要额外确保base必须指向块首页，且不能是保留页。在遍历结束后，设置首页的property属性为n（释放了n页），并更新PG_property为1（此处hint提示“将当前页块标记为已分配状态”或许不太准确？），并增大nr_free。

##### 插入freelist并尝试合并
经过上述设置后，尝试将base插入freelist。逻辑与内存分配环节基本相同。主要区别在链接后的合并尝试：先通过list_prev()取得base在freelist中的前一项base'，base'+base'->property==base，那么说明它们是连续的，可以合并。合并时统一向前合并，合并至base'项，并删除base项。同样地，还需要尝试向后合并，逻辑完全相同。
```cpp=
    if (le != &free_list)
    {
        p = le2page(le, page_link);
        if (p + p->property == base)
        {
            p->property += base->property;
            ClearPageProperty(base);
            list_del(&(base->page_link));
            base = p;
        }
    }

    le = list_next(&(base->page_link));// 以下是向后合并的尝试
    if (le != &free_list)
    {
        p = le2page(le, page_link);
        if (base + base->property == p)
        {
            base->property += p->property;
            ClearPageProperty(p);
            list_del(&(p->page_link));
        }
    }
```

#### 实验结果和可能的改进
实验结果：Total Score: 25/25

可能的改进：

（1）目前双向链表的结构不一定最优：每次分配/释放时都需要从链表头遍历，处于线性时间复杂度，但实际上freelist中元素是按地址排列的，这种查找方式显然没有利用这一点。可以用AVL树将查找的时间复杂度降低至O(logn)级。这个修改较为简单，但这么做也会导致维护AVL树的额外开销，不能排除负优化的可能；

（2）目前的内存管理是单线程的，没有考虑多进程同时分配时的情况，如果发生竞争目前的实现无法解决。完善的实现至少需要使用锁或原子操作，目前实现困难很大。

### Challenge1：Buddy System（伙伴系统）分配算法

Buddy System算法把系统中的可用存储空间划分为存储块(Block)来进行管理, 每个存储块的大小必须是2的n次幂。下面结合关键代码解释伙伴系统的实现过程。

#### 内存初始化

遍历所有页的过程类似best fit，但此时不设置base->property，这是显然的，因为此时并不将这n页视为一整块。

```cpp=
    // init负责初始化MAX_ORDER个freelist，第i个freelist存放2^i页的空闲块
    static void buddy_init(void){
        cprintf("=== BUDDY INIT CALLED ===\n");
        for (int i = 0; i < MAX_ORDER; i++){
            list_init(&(free_area[i].free_list));
            free_area[i].nr_free = 0; // 最初没有空闲块
        }
    }
```

初始化的核心是一个循环，将块分割，对代码的解释可见注释：
```cpp=
    size_t remaining = n;// 最初剩n块待处理
    struct Page *current = base;// 初始current位于base
    while (remaining > 0)
    {
        int max_order = log2_floor(remaining);
        if (max_order >= MAX_ORDER)
        {
            // 此处设置了全局变量，一次性最多分配的页数是固定的
            max_order = MAX_ORDER - 1;
        }

        size_t page_idx = current - pages;
        int order = max_order;// 应该插入到freearea[order]，与前文freelist对应
        size_t block_size = 1 << order;// 切出来块的大小

        // 这里计算对齐：例如page_idx=5时，order计算为2，但这会剩下一个1page大小的块
        // 因此逐步调整到order=0
        // 尽管这种情况好像并没有实际发生，但仍然保留了该判断
        while (order > 0 && (page_idx % block_size) != 0)
        {
            order--;
            block_size = 1 << order;
        }

        // 确保不超过剩余大小
        if (block_size > remaining)
        {
            order = log2_floor(remaining);
            block_size = 1 << order;
        }

        // freearea更新
        current->property = block_size;
        SetPageProperty(current);
        list_add_before(&(free_area[order].free_list), &(current->page_link));
        free_area[order].nr_free++;

        current += block_size;
        remaining -= block_size;
    }
```

#### 内存分配

在伙伴系统中，手动指定了单次最大分配页数。因此申请时要先进行大小判断。如果过大则直接分配失败。假设申请页数为n，首先需要计算大于等于n的最小二次幂size，并将指数order记录，用于freearea的查找。
查找时，从freearea\[order]开始，每次循环尝试找存更大块的freelist。

假设在free_area\[current_order]中找到了这样的一块，块首为page。下面还需要尝试分裂块，直至得到size大小的子块。具体分裂过程如下：

```cpp=
    // 分裂块直到所需大小
    // 每次分裂，左边的块继续分裂直至大小合适，右边的块作为新空闲块加入free_area
    while (current_order > order)
    {
        current_order--;
        size_t buddy_size = 1 << current_order;
        struct Page *buddy = page + buddy_size;// 右边的块放回低一级的freelist
        buddy->property = buddy_size;
        SetPageProperty(buddy);
        // 直接在链表头插入，最后的块合并并不需要在freelist中读取地址，因此插入位置比较随意
        list_add_before(&(free_area[current_order].free_list), &(buddy->page_link));
        free_area[current_order].nr_free++;
    }// 出循环后current_order等于order，此时大小对应size，就可以分配了
```

#### 内存释放

在伙伴系统的内存释放中，比较困难的是伙伴页的判定问题，因此报告重点解释这一部分。

```cpp=
    static int is_buddy_free(struct Page *page, int order){
        struct Page *buddy = get_buddy(page, order);//这里通过偏移找到可能的伙伴块

        // 检查伙伴页是否空闲和大小
        if (!PageProperty(buddy) || buddy->property != (1 << order))return 0;

        // 验证伙伴关系
        /* 这里内存可以近似被视为分割为一个树状结构：如果两个子块确实是伙伴块，那么它们应该有相同的父块。
        用类似索引的思想来判断伙伴关系，两个order级别的伙伴块，其索引应该只在后order+1位有区别
        */
        size_t page_idx = page - pages;
        size_t buddy_idx = buddy - pages;

        size_t parent_mask = ~((1 << (order + 1)) - 1);
        // 如果这两块不只是末尾有区别，那说明它们不是伙伴块。
        if ((page_idx & parent_mask) != (buddy_idx & parent_mask))return 0; 

        // 确保伙伴在对应的自由链表中
        list_entry_t *le;
        for (le = list_next(&(free_area[order].free_list));
             le != &(free_area[order].free_list);
             le = list_next(le))
        {
            struct Page *p = le2page(le, page_link);
            if (p == buddy)return 1; 
        }
        return 0;
    }
```

在解决伙伴页判定问题后，合并块的任务就基本完成了。在尝试合并时，递归地判断合并，每次循环将新块指针指向块首。最后得到尽可能大的块后，更新freearea。
```cpp=
    // 更新对应freelist
    list_entry_t *le = &(free_area[order].free_list);
    while ((le = list_next(le)) != &(free_area[order].free_list))
    {
        struct Page *p = le2page(le, page_link);
        if (base < p)
        {
            break;
        }
    }
    list_add_before(le, &(base->page_link));
    free_area[order].nr_free++;
```

#### 实验结果

目前提供了一些测试样例。包括以下内容：内存管理布局初始化、简单的小页分配和释放（例如1/2/4页）、以及需要分裂的分配情况（本实验中根据freearea设计了17页的内存申请和释放），具体请见buddy_pmm_manager.c。需要指出的是，伙伴合并过程已经在这些样例中得到了测试。对每个样例，都打印了测试前后的freearea情况，实际上通过这个输出值就可以基本判断内存的分配和回收情况。

### Challenge2：任意大小的内存单元slub分配算法

#### Slub原理
SLUB（Slab Utilization By-pass）是 Linux 内核中用于管理小块内存分配（kernel object allocation）的一种 高效 slab 分配器。
它是对早期 SLAB 分配器的简化与优化版本，目标是：降低内存碎片；减少锁竞争；提高并发性能；简化代码结构和调试逻辑。

#### Slub基本概念
##### 缓存kmem_cache
每种类型的对象（如 task_struct）对应一个 slab cache。
定义在结构体：
```cpp=
struct kmem_cache {
    unsigned int size;         // 对象大小
    unsigned int align;        // 对齐要求
    unsigned int object_size;  // 实际对象大小
    struct kmem_cache_cpu *cpu_slab;  // 每CPU缓存
    struct kmem_cache_node *node[MAX_NUMNODES]; // NUMA节点缓存
    ...
};
```

##### slab
每个 slab 是一块连续的物理内存页（通常为 1~8 页），用于存放多个对象。
每个 slab 中：对象是等大小的；通过链表或位图标记已分配与空闲对象。

##### kmem_cache_cpu
每个 CPU 有自己的小缓存（per-CPU cache），用来减少锁竞争。
结构体如下：
```cpp=
struct kmem_cache_cpu {
    void **freelist;   // 当前可分配对象的空闲链表
    struct page *page; // 当前正在使用的 slab
};
```

##### Slub分配流程
以 kmem_cache_alloc() 为例。

步骤 1：从当前 CPU 的 freelist 获取对象

每个 CPU 都有一个私有的 freelist（无需锁）。

若有空闲对象，直接弹出并返回 → 最快路径（lockless fast path）。

步骤 2：若 freelist 为空，从当前 CPU 的 page 中补充对象

page 指向该 CPU 当前正在使用的 slab。

如果 page 还有可用对象，则更新 freelist。

否则，进入下一步。

步骤 3：若当前 slab 已满，从 NUMA 节点上分配新的 slab

通过 kmem_cache_node 获取一个新 slab（通常来自伙伴系统）。

初始化其对象链表。

将该 slab 绑定到当前 CPU 的 kmem_cache_cpu.page。

继续分配对象。

步骤 4：若 NUMA 节点也无可用 slab，则从全局分配新的页。

##### Slub释放流程

将对象重新插入当前 CPU 的 freelist；

如果当前 slab 空闲率过高（比如超过一定阈值），则把它放回 node 级的部分空闲 slab 列表；

若整个 slab 中的对象都释放完，则将该 slab 归还伙伴系统。

这同样是一个分层回收机制，避免频繁操作全局结构。
### Challenge3: 硬件的可用物理内存范围的获取方法

由 Bootloader 在启动时解析硬件信息， 将可用物理内存范围（通常是 DRAM 起始地址与大小） 保存到 设备树（DTB） 或类似结构中， 内核启动后读取并解析其中的 memory 节点， 从而获得可用物理内存区间。