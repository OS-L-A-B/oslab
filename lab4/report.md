---
title: lab4：进程管理
author: 钱俊玮 朱荟宇 邹博闻
---
# <center>lab4:进程管理</center>

#### <center> 钱俊玮&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;朱荟宇&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;邹博闻 </center>
#### <center> 2312480&nbsp;2311824&nbsp;2312251 </center>

### exercise 练习1：分配并初始化一个进程控制块

alloc_proc函数（位于kern/process/proc.c中）负责分配并返回一个新的struct proc_struct结构，用于存储新建立的内核线程的管理信息。以下是初始化过程：

```cpp
static struct proc_struct *alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc != NULL)
    {
        proc->state = PROC_UNINIT;                  
        proc->pid = -1;                             
        proc->runs = 0;                             
        proc->kstack = 0;                           
        proc->need_resched = 0;                    
        proc->parent = NULL;                        
        proc->mm = NULL;                           
        memset(&(proc->context), 0, sizeof(struct context));  
        proc->tf = NULL;                          
        proc->pgdir = boot_pgdir_pa;               
        proc->flags = 0;                            
        memset(proc->name, 0, PROC_NAME_LEN + 1); 
    }
    return proc;
}
```

PCB初始化过程需要完善进程信息，具体信息可以参照proc_struct结构体。
PROC_UNINIT=0表示初始化未完全完成（后续会在do_fork中wakeup），pid=-1表示没有分配PID，runs=0表示没有被调度执行过。
由于内核栈需要动态分配，所以kstack=0（在do_fork中调用setup实际分配）。

need_resched=0，表示不需要重新调度（参考trap.c中，如果时间片用完，将其置1）。
Lab4中只实现内核线程，所以mm被初始化为NULL。
alloca时并没有上下文需要保存，context初始化为0，在进程复制（copy_thread）时会设置ra和sp。
trapframe在内核栈上分配，但此时内核栈并没有分配，所以tf初始化为NULL。



struct context context的作用阅读代码即可明确。首先看context本身的结构体信息，它包含一系列uintptr_t（即unsigned long int）。也就是说大概率和进程内容有关；然后发现在proc.c的switch_to中被调用，推测与进程切换时的内容有关。顺逻辑检查switch.S，发现switch_to具体的内容，这个.S文件的功能很明确，即将ra、sp、s0等保存至from->context，再将to->context加载到ra、sp...，实现进程切换。

当触发进程切换时（假设是进程proc1切换到proc2），由proc_run函数可知，此时修改current指针切换当前进程，调用lcr3切换页表，然后调用switch_to。调用后，将proc1的ra等信息保存到proc1->context中，并将proc2->context取出并恢复，让proc2从恢复后的现场继续执行。

struct trapframe的功能与中断有关。阅读trap.h就会发现内容与lab3中断处理的trapframe高度相似，只是将现场信息用struct pushregs记录，同时实现status寄存器，epc，坏地址寄存器，cause寄存器（这与操作系统/体系结构的内容都是一致的）。在trapentry.S中，SAVE_ALL后jal trap，此时栈内的现场信息与结构体是完全对应的，可以直接通过返回的trapframe指针读取。

从中断返回时，proc.c的forkret函数调用forkrts(current->tf)，而fork_rets的工作可以在trapentry.S中找到：

```
forkrets:
    # set stack to this new process's trapframe
    move sp, a0
    j __trapret
```

forkrets跳转到RESTORE_ALL，恢复现场。

总的来看，这两者都与进程切换有关。当时钟中断触发调度时，按照lab3的流程，经过trap_dispatch函数后明确是时钟中断。接着判断current->need_resched。如果需要调度，则触发schedule()。schedule关中断后，将need_resched置0，从进程列表中选择下一个RUNNABLE进程，调用proc_run()开始切换。proc_run()会调用switch_to(&(prev->context), &(next->context))，这个过程中将现场存到prev->context中，将next->context恢复。

由于a0-a7,t0-t6寄存器是调用者保存的，也就是说在switch_to前，这些寄存器的值已经被存到当前栈上了。context只需要存ra、sp、s0-s11即可。所以context只需要14个寄存器。
### exercise 练习2：为新创建的内核线程分配资源

在创建内核线程时，通过 kernel_thread 函数对内核线程分配资源。而 kernel_thread中则是通过 do_fork 函数来完成实际的现成创建和资源分配。

而exercise2 的内容就是补全 do_fork 函数中关于线程创建和资源分配的代码。下面根据具体代码和练习要求的流程进行解释。

```cpp
    // alloc_proc有可能输出NULL，需要考虑一下
    if ((proc = alloc_proc()) == NULL){
        goto fork_out;
    }
    // 设置父进程
    proc->parent = current;
    if(setup_kstack(proc) < 0){
        goto bad_fork_cleanup_proc;
    }
    if (copy_mm(clone_flags, proc) < 0){
        goto bad_fork_cleanup_kstack;
    }
    copy_thread(proc, stack, tf);
    proc->pid = get_pid();
    hash_proc(proc);
    list_add(&proc_list, &(proc->list_link));
    wakeup_proc(proc);
    nr_process++;
    ret = proc->pid;

fork_out:
    return ret;

bad_fork_cleanup_kstack:
    put_kstack(proc);
bad_fork_cleanup_proc:
    kfree(proc);
    goto fork_out;
```
`fork_out`及之后的代码是框架已经实现好的返回逻辑和释放资源逻辑，这里贴上是因为需要用到便于解释。

首先调用 alloc_proc 获取一个用户信息块，其实就是一个初始化好的空进程结构体。因为可能输出NULL，所以需要判断并处理。如果是NULL，便直接返回。成功拿到一个进程块时，设置它的父进程为当前进程。

通过调用 setup_kstack 为这个进程块分配一个内核栈。同样可能分配失败，需要判断并处理。因为此时有进程空间，而没有分配内核栈空间，所以只需要释放进程空间即可。

再通过调用 copy_mm 复制原进程的内存管理信息给新进程。通过 copy_thread 复制原进程上下文到新进程。

进程资源分配完毕，先给进程分配新进程号，再将进程通过 hash_proc 和 list_add 分别加入`hash_list`和`proc_list`，前者是用于查找进程的哈希桶，后者则是链接所有进程的进程集合。

最后再用 wakeup_proc 唤醒进程。框架里似乎没有实现 wakeup_proc，于是我就自己写了一个，作用很简单，就是调整进程状态为可运行。
```cpp
#define wakeup_proc(proc) (proc->state = PROC_RUNNABLE)
```
最后，给总进程数加一，然后设置返回值为这个进程的pid值。完成内核线程的创建和资源分配。

#### 问题回答

exercise2中提到如下问题：
>请说明ucore是否做到给每个新fork的线程一个唯一的id？请说明你的分析和理由。

我的回答是能做到。程序中使用 get_pid 函数来获取新的pid，我们来分析它的实现。
```cpp
// get_pid - alloc a unique pid for process
static int
get_pid(void)
{
    static_assert(MAX_PID > MAX_PROCESS);
    struct proc_struct *proc;
    list_entry_t *list = &proc_list, *le;
    static int next_safe = MAX_PID, last_pid = MAX_PID;
    if (++last_pid >= MAX_PID)
    {
        last_pid = 1;
        goto inside;
    }
    if (last_pid >= next_safe)
    {
    inside:
        next_safe = MAX_PID;
    repeat:
        le = list;
        while ((le = list_next(le)) != list)
        {
            proc = le2proc(le, list_link);
            if (proc->pid == last_pid)
            {
                if (++last_pid >= next_safe)
                {
                    if (last_pid >= MAX_PID)
                    {
                        last_pid = 1;
                    }
                    next_safe = MAX_PID;
                    goto repeat;
                }
            }
            else if (proc->pid > last_pid && next_safe > proc->pid)
            {
                next_safe = proc->pid;
            }
        }
    }
    return last_pid;
}
```
这个函数中首先设置了静态变量`next_safe`和`last_pid`，我理解为`当前大于last_pid的可用的pid的边界`和`当前能返回的pid`。

整个程序的思路是，初次调用边界设置为`MAX_PID`，从1开始向下分配。每次调用都先将`last_pid`加一，超过`MAX_PID`就回到1，循环分配。如果`last_pid < next_safe`，自然直接返回。否则遍历所有的进程，重新找到当前的正确的`next_safe`。遍历时，如果：
 - **所有的进程的pid都比`last_pid`小**，那就直接返回；
 - **如果有进程的pid大于`last_pid`并且小于`next_safe`**，这说明`next_safe`太大，收缩到当前这个pid，继续遍历；
 - **如果存在有进程的pid刚好等于`last_pid`**，意味着当前的`last_pid`已经不能使用，需要重新设置。首先，`last_pid`自增，如果小于`next_safe`，说明此时`next_safe`的寻找没有被破坏，可以直接继续遍历。反之，`next_safe`又要回到`MAX_PID`，重新遍历所有的进程再次开始寻找正确的`next_safe`。当然，每次`last_pid`自增都需要考虑有没有超过`MAX_PID`，超过了就回到1继续找。

根据这个流程可以看到，只要能完成所有进程的遍历，就一定能拿到正确的`last_pid`和`next_safe`，所以必然唯一。

### exercise 练习3：编写proc_run 函数
```cpp
    void proc_run(struct proc_struct *proc){
        if (proc != current){
            // LAB4:EXERCISE3 YOUR CODE
            /*
             * Some Useful MACROs, Functions and DEFINEs, you can use them in below implementation.
             * MACROs or Functions:
             *   local_intr_save():        Disable interrupts
             *   local_intr_restore():     Enable Interrupts
             *   lsatp():                   Modify the value of satp register
             *   switch_to():              Context switching between two processes
             */
            bool intr_flag;
            local_intr_save(intr_flag);        // 关中断

            struct proc_struct *prev = current;
            current = proc;                    // 切换当前进程

            // 切换页表（地址空间）
            lsatp((unsigned int)proc->pgdir);
            asm volatile("sfence.vma zero, zero" ::: "memory");

            // 上下文切换
            switch_to(&(prev->context), &(proc->context));

            local_intr_restore(intr_flag);     // 开中断
        }
    }
```

我们可以在libs/risv.h中找到lsatp的函数定义
```cpp
    static inline void
    lsatp(unsigned int pgdir){
      write_csr(satp, SATP32_MODE | (pgdir >> RISCV_PGSHIFT));//satp 需要的不是字节地址，而是 PPN（页号），即页表地址右移12位的值
                                                              //#define RISCV_PGSHIFT 12
    }
```

我们可以在kerns/process/swich.S中找到switch_to函数定义
```cpp
    .text
    # void switch_to(struct proc_struct* from, struct proc_struct* to)
    .globl switch_to
    switch_to:
        # save from's registers
        STORE ra, 0*REGBYTES(a0)
        STORE sp, 1*REGBYTES(a0)
        STORE s0, 2*REGBYTES(a0)
        STORE s1, 3*REGBYTES(a0)
        STORE s2, 4*REGBYTES(a0)
        STORE s3, 5*REGBYTES(a0)
        STORE s4, 6*REGBYTES(a0)
        STORE s5, 7*REGBYTES(a0)
        STORE s6, 8*REGBYTES(a0)
        STORE s7, 9*REGBYTES(a0)
        STORE s8, 10*REGBYTES(a0)
        STORE s9, 11*REGBYTES(a0)
        STORE s10, 12*REGBYTES(a0)
        STORE s11, 13*REGBYTES(a0)

        # restore to's registers
        LOAD ra, 0*REGBYTES(a1)
        LOAD sp, 1*REGBYTES(a1)
        LOAD s0, 2*REGBYTES(a1)
        LOAD s1, 3*REGBYTES(a1)
        LOAD s2, 4*REGBYTES(a1)
        LOAD s3, 5*REGBYTES(a1)
        LOAD s4, 6*REGBYTES(a1)
        LOAD s5, 7*REGBYTES(a1)
        LOAD s6, 8*REGBYTES(a1)
        LOAD s7, 9*REGBYTES(a1)
        LOAD s8, 10*REGBYTES(a1)
        LOAD s9, 11*REGBYTES(a1)
        LOAD s10, 12*REGBYTES(a1)
        LOAD s11, 13*REGBYTES(a1)

        ret
```
该函数保存了from进程的寄存器，恢复to进程的寄存器

### 回答问题
我们创建并运行了两个内核进程，一个是idleproc（线程名：idel，即空转线程，负责在没有其它线程运行时负责让CPU空闲），一个是initproc（线程名：init，运行 init_main，即输出"Hello world!"）

### challenge1：说明语句local_intr_save(intr_flag);....local_intr_restore(intr_flag);是如何实现开关中断的

实际上对照代码来看，这个过程还是比较清晰的。local_intr_save调用__intr_save()，intr_save()依次完成以下操作：

（1）通过read_csr()读取sstatus寄存器;
（2）如果读取到了，且SIE位=1（中断开启），则调用intr_disable()并返回1，否则返回0。

而intr_disable()调用clear_csr(sstatus, SSTATUS_SIE)，这个宏位于libs/riscv.h中：

```cpp
#define clear_csr(reg, bit) ({ unsigned long __tmp; \
  asm volatile ("csrrc %0, " #reg ", %1" : "=r"(__tmp) : "rK"(bit)); \
  __tmp; })
```

执行时执行csrrc t0, sstatus, SSTATUS_SIE，将sstatus读取到rd，清零rs1位，再写回scr，即实现关中断。

开中断的过程可以类似地理解，注释中进行了解释。
```cpp
static inline void __intr_restore(bool flag) {
    if (flag) {
        intr_enable();
    }
}
// intr.c
void intr_enable(void) { set_csr(sstatus, SSTATUS_SIE); }
// libs/riscv.h，与clear_csr高度类似，只是此处是置位而不是清零
#define set_csr(reg, bit) ({ \
    unsigned long __tmp; \
    asm volatile ("csrrs %0, " #reg ", %1" : "=r"(__tmp) : "r"(bit)); \
    __tmp; })
```

### challenge2：深入理解不同分页模式的工作原理

以下两个问题围绕get_pte()函数展开，注释中对代码进行了解释：

```cpp
// 求出线性地址对应的页表项
pte_t *get_pte(pde_t *pgdir, uintptr_t la, bool create)
{
    pde_t *pdep1 = &pgdir[PDX1(la)];
    // 把la右移30位，再与上1FF，取出la的高9位，即VPN[2]
    if (!(*pdep1 & PTE_V))
    {
        // 此时一级页表项无效，二级页表不应该存在
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        // 获取新页的physical address
        uintptr_t pa = page2pa(page);
        // KADDR：pa转虚拟地址
        memset(KADDR(pa), 0, PGSIZE);// 新页清零
        *pdep1 = pte_create(page2ppn(page), PTE_U | PTE_V);
        // ppn：获取物理页号，PTE_U: 用户可访问，PTE_V: 页表项有效
    }
    pde_t *pdep0 = &((pte_t *)KADDR(PDE_ADDR(*pdep1)))[PDX0(la)];
    // 注意PDX0是右移21位再与1FF，拿到的是二级页目录项指针！
    if (!(*pdep0 & PTE_V))
    {
        // 与前文类似
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE);
        *pdep0 = pte_create(page2ppn(page), PTE_U | PTE_V);
    }
    return &((pte_t *)KADDR(PDE_ADDR(*pdep0)))[PTX(la)];
}
```

**问题1**：get_pte()函数中有两段形式类似的代码， 结合sv32，sv39，sv48的异同，解释这两段代码为什么如此相像。

sv39的虚拟地址结构为9+9+9+12（页内偏移），前三个分别为一/二/三级页表索引。但无论是sv32还是sv39、sv48，la找对应页表项的过程是相似的：从虚拟地址提取当前级页表的索引，然后检查页表项是否有效，再返回下一级页表的地址。

两段代码的主要区别是索引来源不同，第一段是取la的第31-39位，而第二段是取第22-30位。如果是sv48，理论上再添加一段相似的代码即可。代码的相似是由la结构所决定的

**问题2**：目前get_pte()函数将页表项的查找和页表项的分配合并在一个函数里，你认为这种写法好吗？有没有必要把两个功能拆开？

目前这种写法，一次调用就可以完成页表项的查找/分配，通过create控制具体应该做什么。这种写法有一个明显好处：在调用时create传1，如果不存在就会直接分配，这样如果分配成功pte就能直接使用。同时一个函数内部调用相比多个函数调用在竞争问题上相对安全。

但在这种写法下，当return NULL时，会对应两种情况：create=0时页表项不存在，或者create=1时分配失败。在调用时具体原因还需要根据create判断，否则语义并不明确。