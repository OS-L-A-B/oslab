---
title: lab5：用户程序
author: 钱俊玮 朱荟宇 邹博闻
---
# <center>lab5：用户程序</center>

#### <center> 钱俊玮&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;朱荟宇&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;邹博闻 </center>
#### <center> 2312480&nbsp;2311824&nbsp;2312251 </center>

### exercise 练习1：加载应用程序并执行
#### 代码和解释
exercise1的主要任务是完成 load_icode 函数第6步，设置进程的trapframe结构。do_execve 函数会调用 load_icode 来加载并解析内存中的ELF格式可执行文件。
主要代码见下：
```cpp
//(6) setup trapframe for user environment
struct trapframe *tf = current->tf;
uintptr_t sstatus = tf->status;
memset(tf, 0, sizeof(struct trapframe));

// 设置用户栈指针
tf->gpr.sp = USTACKTOP;

// 设置程序入口点
tf->epc = elf->e_entry;

// 设置状态寄存器
tf->status = ((read_csr(sstatus) & ~SSTATUS_SPP) | SSTATUS_SPIE) & ~SSTATUS_SIE;
```
下对tf的信息进行解释。sp和epc比较简单。首先是sp，系统在前面开辟了用户栈，因此此时将sp指向用户栈栈顶，用户程序可以访问栈内信息；tf->epc被设置为elf文件的入口地址。sret返回用户态时，CPU将PC设置为 epc 的值，从而跳转到用户程序的第一条指令。

对status的设置略复杂。首先是read_csr宏，这个宏在LAB4中有涉及，定义在riscv.h中，主要是通过csrr读取status寄存器。首先是清楚SSTATUS_SPP位。当SPP=0时，sret返回用户态，这里强制清零，确保不会回到内核态；然后是SPIE位置为1，确保用户程序运行时可以响应中断。

最后是清零SIE位。最然sret后还是SIE位还是会被SPIE覆盖为1，但目前还是暂时清零了，避免返回用户态的过程中被中断打断。

#### 用户态进程被ucore选择占用CPU执行（RUNNING态）到具体执行应用程序的整个经过

在init_main中，调用了kernel_thread(user_main,NULL,0)，这个函数将tf.gpr.s0指向user_main，s1置为NULL，epc置为kernel_thread_entry，然后do_fork。
```cpp
int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags) {
    struct trapframe tf;
    memset(&tf, 0, sizeof(struct trapframe));
    tf.gpr.s0 = (uintptr_t)fn;          // user_main 函数地址
    tf.gpr.s1 = (uintptr_t)arg;
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;
    tf.epc = (uintptr_t)kernel_thread_entry;  // 入口设为 kernel_thread_entry
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}
```
do_fork在之前的实验中涉及，比较重要的是调用了copy_thread，这个函数将返回地址指向forkret，将context.sp指向tf。

此后，当schedule选中user_main后，schedule调用proc_run切换进程。proc_run中的switch_to会跳转到forkret：
```cpp
static void forkret(void) {
    forkrets(current->tf);
}
```
而forkrets等最终会sret，而刚才tf->epc=kernel_thread_entry，因此返回到该处，并jalr s0（即user_main），开始执行user_main。

user_main中用宏KERNEL_EXECVE(exit)加载用户程序，调用kernel_execve函数。这个函数触发断点异常ebreak，然后进入exception_handler的CAUSE_BREAKPOINT分支：
```cpp
case CAUSE_BREAKPOINT:
        cprintf("Breakpoint\n");
        if (tf->gpr.a7 == 10)
        {
            tf->epc += 4;
            syscall();
            kernel_execve_ret(tf, current->kstack + KSTACKSIZE);
        }
        break;
```
syscall会调用do_execve→load_icode，而load_icode就负责加载解析elf。由于正常return都是0，因此syscall将返回值写入tf->gpr.a0，然后trap返回。trapret中最后sret，而此时epc已经在load_icode中设置了，程序开始执行第一条指令。

以上是比较完整的调用链，尽可能详细地指出了调用过程，前后基本是可以流畅理解的。

### exercise 练习2：父进程复制自己的内存空间给子进程 
```cpp
int copy_range(pde_t *to, pde_t *from, uintptr_t start, uintptr_t end,bool share)
    //to：子进程页目录
    //from：父进程页目录
    //[start, end)：要复制的虚拟地址范围
    //share：表示是否共享页
{
    assert(start % PGSIZE == 0 && end % PGSIZE == 0);
    assert(USER_ACCESS(start, end));
    // copy content by page unit.
    do
    {
        // 用 get_pte 查父进程在地址 start 的 PTE
        //create=0 表示不创建新页表项
        pte_t *ptep = get_pte(from, start, 0), *nptep;
        
        //PTE 不存在意味着：
        // 对应的 4KB 页面在父进程未映射
        // 则直接跳过该页表所在的整个 4MB 范围
        if (ptep == NULL)
        {
            start = ROUNDDOWN(start + PTSIZE, PTSIZE);
            continue;
        }
        
        if (*ptep & PTE_V)
        {
            //子进程页表中如果缺失相应的页表结构，则自动创建
            //create=1，需要时创建页表
            if ((nptep = get_pte(to, start, 1)) == NULL)
            {
                return -E_NO_MEM;
            }
            
            //保留用户位标志，新的页需要使用相同权限
            uint32_t perm = (*ptep & PTE_USER);
            //找到父进程页面
            struct Page *page = pte2page(*ptep);
            //子进程要得到一个 全新的页面
            struct Page *npage = alloc_page();
            assert(page != NULL);
            assert(npage != NULL);
            int ret = 0;
            
            // (1) 获取源页面的内核虚拟地址
            void *src_kvaddr = page2kva(page);

            // (2) 获取目标页面的内核虚拟地址  
            void *dst_kvaddr = page2kva(npage);

            // (3) 从源页面复制内容到目标页面
            memcpy(dst_kvaddr, src_kvaddr, PGSIZE);

            // (4) 建立目标页面与线性地址的映射
            ret = page_insert(to, npage, start, perm);
            
            // (5) 记录虚拟地址
            npage->pra_vaddr = start;

            assert(ret == 0);
        }
        start += PGSIZE;
    } while (start != 0 && start < end);
    return 0;
}
```
COW机制在challenge1中进行设计说明

### exercise3 理解进程执行 fork/exec/wait/exit 的实现，以及系统调用的实现

#### fork系统调用
syscall.c中可以看到：
```cpp
int sys_fork(void) {
    return syscall(SYS_fork);
}
```
调用ecall触发调用：
```cpp
void exception_handler(struct trapframe *tf) {
    switch (tf->cause) {
        case CAUSE_USER_ECALL:
            tf->epc += 4;  // 跳过 ecall 指令
            syscall();     // 调用系统调用处理函数
            break;
        // ...
    }
}
```
在syscall中，根据a0寄存器的系统调用号分发到具体的函数sys_fork。然后就是tf和sp的记录并调用do_fork。

do_fork的返回值存入tf.a0，然后sret返回用户态后，父进程从fork()处继续执行。

#### 其他系统调用
实际上exec/wait/exit陷入内核的方式都是一样的（通过ecall），只不过syscall分发到的函数不一样。do_exit稍有区别，它不会返回，因为进程已经变成Zombie态，最终由父进程的do_wait()完成清理。

根据这四个类似的系统调用，不难发现比较通用交互方式：
（1）用户程序ecall，由CPU硬件自动保存epc（pc），SPP(当前特权级)，SPIE(SIE)，SIE（0）等。
（2）trap（）函数保存tf，然后跳转handle。对于系统调用，最终会分发到 do_fork 等内核函数，这些函数可以访问进程的所有内核数据结构。
（3）内核函数执行完毕后，返回值被存入 trapframe.a0。trap() 返回后，汇编代码恢复所有寄存器，执行 sret 指令：PC ← epc，特权级 ← sstatus.SPP，sstatus.SIE ← sstatus.SPIE，sstatus.SPIE ← 1。

一个用户态进程的执行状态生命周期图如下：

        alloc_proc()
            |
            v
     +-------------+
     | PROC_UNINIT |  (尚未初始化完成)
     +-------------+
            |
            | proc_init() / wakeup_proc()
            v
     +---------------+
     | PROC_RUNNABLE | <--------------------+
     +---------------+                      |
            |                               |
            | schedule() 选中               | wakeup_proc()
            v                               |
       【RUNNING】                          |
        (占用CPU)                           |
            |                               |
            +-------+-------+-------+       |
            |       |       |       |       |
         do_wait do_sleep  时间   I/O等    |
         /do_exit  /其他  片用完   待完成   |
            |       |       |       |       |
            v       v       |       |       |
     +---------------+      |       |       |
     | PROC_SLEEPING | -----+-------+-------+
     +---------------+  (等待事件发生后被唤醒)
            
            
     【RUNNING】
        (执行过程中)
            |
            | do_exit(error_code)
            v
     +-------------+
     | PROC_ZOMBIE |  (等待父进程回收)
     +-------------+
            |
            | 父进程调用 do_wait()
            v
        进程资源回收，PCB被释放
        
### challenge1：实现 Copy on Write （COW）机制

COW机制是对父进程创建子进程时对子进程内存空间的一种管理机制。传统的方法在复制了父进程的虚拟内存结构给子进程后，会创建新的物理页给子进程使用。而COW机制则是子进程和父进程共享父进程的物理空间，并且将原来的物理空间标记为只读。如果某个进程发生写操作，则对写的地方复制一个新的物理页供其使用。

基于此，我们首先需要修改物理页中处理复制物理空间的函数 copy_range。因为原来就有 share 参数作为是否共享的记号，所以只看 share 分支的代码，其他部分和原来一致。

```cpp
// === COW 分支 (新加的) ===

// 1. 修改权限：加上 COW 标记，强制去掉 Write 权限
perm |= PTE_COW;
perm &= ~PTE_W;
// 【插入这行】
// 打印看看：地址是多少？权限是多少？(期待看到 perm 里没有 PTE_W)
// PTE_W 通常是 4 (二进制 100)，PTE_COW 是 0x100
// 如果打印出的 perm 是 0x10x (比如 0x107 -> User|Read|COW)，那就是对的。
// 如果打印出 0x1x7 (比如 0x10b -> User|Write|Read|COW)，那就是 PTE_W 没去干净！
if (start < 0xC0000000) { // 只打印用户态地址，防止刷屏
    cprintf("COW SETUP: addr=0x%x, perm=0x%x\n", start, perm);
}

// 2. 映射给子进程 (to)
// page_insert 会自动增加引用计数 (page->ref++)
if (page_insert(to, page, start, perm) != 0) {
    return -E_NO_MEM;
}

// 3. 【关键】修改父进程 (from) 的权限
// 父进程也必须变成只读+COW，否则父进程一写，子进程的数据就变了
// page_insert 发现是同一个页映射同一个地址，只会更新 PTE 权限，不会错误地增加 ref
if (page_insert(from, page, start, perm) != 0) {
    return -E_NO_MEM;
}
```

修改页权限去掉写权限，加上COW标记，然后将这个页用 page_insert 同时映射给子进程和父进程。PTE_COW 定义为 0x100，是一个保留位。

而进程发生写操作时，会触发 page fault 来进行新的页复制。所以我们需要写 page fault的处理函数 do_pgfault。

```cpp
int do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr){
    int ret = -E_INVAL;

    // --- Part 1: 合法性检查 ---
    // 查找包含 addr 的 VMA (虚拟内存区域)
    struct vma_struct *vma = find_vma(mm, addr);

    // 1. 如果地址没落在任何 VMA 范围内，说明程序访问了野指针，直接报错
    if (vma == NULL || addr < vma->vm_start) {
        cprintf("Invalid addr %x\n", addr);
        return -E_INVAL;
    }

    // 2. 检查权限
    // RISC-V 中，error_code 就是 cause。
    // CAUSE_STORE_PAGE_FAULT 的值是 15 (在 riscv.h 中定义，或者直接写 15)
    // 如果是写操作触发的异常，但 VMA 本身标记为不可写，那也是非法访问
    // 注意：这里我们先不管 COW，只看 VMA 这一层的大逻辑
    // 标志位定义在 vmm.h: VM_WRITE, VM_READ 等
    if ((error_code == CAUSE_STORE_PAGE_FAULT) && !(vma->vm_flags & VM_WRITE)) {
         cprintf("Write access to read-only vma!\n");
         return -E_INVAL;
    }
    
    // --- 准备工作 ---
    // 把权限位转成 PTE 格式，准备一会给页表用
    uint32_t perm = PTE_U;
    if (vma->vm_flags & VM_WRITE) {
        perm |= (PTE_R | PTE_W);
    }
    // 把地址对齐到页边界 (4KB)
    addr = ROUNDDOWN(addr, PGSIZE);
    
    ret = -E_NO_MEM; // 默认返回内存不足错误
    pte_t *ptep = NULL;
    
    // 获取页表项 (第三个参数 0 表示如果页表不存在，不自动创建，只返回 NULL)
    // 我们先看看原来有没有映射，以此判断是 COW 还是 第一次访问
    ptep = get_pte(mm->pgdir, addr, 0);


    // --- Part 2: COW 核心逻辑 (我们要填的地方) ---
    // 只有同时满足以下条件，才是 COW：
    // 1. 页表项存在 (*ptep != 0)
    // 2. 也是有效的 (*ptep & PTE_V)
    // 3. 是写操作触发的 (error_code == 15)
    // 4. 页表项里有我们打的 PTE_COW 标记
    if (ptep && (*ptep & PTE_V) && (error_code == CAUSE_STORE_PAGE_FAULT) && (*ptep & PTE_COW)) {
        
        // 【这里是你实现 COW 的地方】
        // 伪代码逻辑：
        // 1. alloc_page() 申请新页
        // 2. memcpy() 拷贝旧页内容
        // 3. page_insert() 建立新映射，并赋予 PTE_W 权限
        

        cprintf("COW TRIGGERED: addr=0x%x, pid=%d\n", addr, current->pid);
        struct Page *page = pte2page(*ptep); // 获取当前旧的物理页
        
        // 计算新页面的权限：
        // 既然是 COW 进来的，说明 VMA 肯定是可写的 (VM_WRITE)。
        // 所以新页面的权限应该是：用户态(PTE_U) | 可写(PTE_W) | 有效(PTE_V)
        // 注意：这里绝对不能再有 PTE_COW 了！
        uint32_t perm = PTE_U | PTE_W | PTE_V; 

        // 2. 【优化】: 如果这个物理页的引用计数只有 1
        // 说明我是唯一的拥有者（比如子进程已经退出了，或者子进程已经 copy 走了）。
        // 那我就不需要费劲申请新页和拷贝了，直接把自己扶正即可。
        if (page_ref(page) == 1) {
            
            // 修改 PTE：去掉 PTE_COW，加上 PTE_W
            *ptep &= ~PTE_COW;
            *ptep |= PTE_W;
            
            // 别忘了刷新 TLB！因为 CPU 缓存里可能还记着它是只读的。
            tlb_invalidate(mm->pgdir, addr);
            cprintf("COW optimized for refcount 1!\n");
            
            return 0; // 搞定收工
        }

        // 3. 【复制】: 标准流程 (ref > 1)
        // 说明还有别的人（比如父进程）也在用这个页，我得独立出来。

        // A. 申请一块新的物理页
        struct Page *npage = alloc_page();
        if (npage == NULL) {
            return -E_NO_MEM; // 内存不足，这就没办法了
        }

        // B. 拷贝数据
        // page2kva 把物理页转成内核虚拟地址，方便 memcpy
        void * src_kvaddr = page2kva(page);
        void * dst_kvaddr = page2kva(npage);
        memcpy(dst_kvaddr, src_kvaddr, PGSIZE);

        // C. 建立新映射 (偷天换日)
        // 这一步非常关键！page_insert 帮我们做了三件事：
        //   1. 把虚拟地址 addr 指向了 npage。
        //   2. 把 npage 的引用计数 +1。
        //   3. 【自动】把原来旧 page 的引用计数 -1 (因为它发现这里原来有映射)。
        if (page_insert(mm->pgdir, npage, addr, perm) != 0) {
            free_page(npage);
            return -E_NO_MEM;
        }
        cprintf("COW handled by copying page!\n");

        // page_insert 内部会自动刷新 TLB，所以这里不需要手动 tlb_invalidate
        // 但为了保险起见，或者根据 ucore 具体实现，手动加一个也没错。
        
        return 0; // 搞定收工
    }


    // --- Part 3: 普通缺页处理 (First Access) ---
    // 如果走到这里，说明不是 COW，而是“这页内存从来没分配过”
    // 比如你 malloc 了一块内存，第一次去读写它，就会进到这里。
    
    // pgdir_alloc_page 会申请一个物理页，并建立映射
    if (pgdir_alloc_page(mm->pgdir, addr, perm) == NULL) {
        cprintf("pgdir_alloc_page failed\n");
        return -E_NO_MEM;
    }

    return 0;
}
```

这里做两件事，一个是对于非 COW 的缺页处理，如果访问的物理页确实可写，就正常分配一个新的物理页；另一个就是对 COW 的处理，引用计数为1的时候直接修改权限；其他情况就复制一个新的物理页给写的进程。

我们基于 proc.c 已经设定好的 exit 程序来看 COW的效果。

首先看到 exit.c 的内容：

```cpp
#include <stdio.h>
#include <ulib.h>

int magic = -0x10384;

int
main(void) {
    int pid, code;
    cprintf("I am the parent. Forking the child...\n");
    if ((pid = fork()) == 0) {
        cprintf("I am the child.\n");
        yield();
        yield();
        yield();
        yield();
        yield();
        yield();
        yield();
        exit(magic);
    }
    else {
        cprintf("I am parent, fork a child pid %d\n",pid);
    }
    assert(pid > 0);
    cprintf("I am the parent, waiting now..\n");

    assert(waitpid(pid, &code) == 0 && code == magic);
    assert(waitpid(pid, &code) != 0 && wait() != 0);
    cprintf("waitpid %d ok.\n", pid);

    cprintf("exit pass.\n");
    return 0;
}
```

父进程创建子进程，然后输出一些消息，等待子进程退出，然后自己再退出。下面是exit程序执行时的日志：

```bash
kernel_execve: pid = 2, name = "exit".
Breakpoint
I am the parent. Forking the child...
COW SETUP: addr=0x7fffc000, perm=0x11b
COW SETUP: addr=0x7fffd000, perm=0x11b
COW SETUP: addr=0x7fffe000, perm=0x11b
COW SETUP: addr=0x7ffff000, perm=0x11b
COW SETUP: addr=0x801000, perm=0x113
COW SETUP: addr=0x800000, perm=0x11b
PAGE FAULT: Store fault at 0x7fffff5c
COW TRIGGERED: addr=0x7ffff000, pid=2
COW handled by copying page!
PAGE FAULT: Store fault at 0x7fffff5c
I am parent, fork a child pid 3
I am the parent, waiting now..
PAGE FAULT: Store fault at 0x7fffff5c
COW TRIGGERED: addr=0x7ffff000, pid=3
COW optimized for refcount 1!
I am the child.
waitpid 3 ok.
exit pass.
```

首先分配前的输出表明现在是父进程。后面开始fork，开始对父进程的物理页设定为共享，可以看到权限码为0x11b/0x113，包含 PTE_COW（0x100），不包含 PTE_W（0x004）。说明此时物理页共享成功。

然后父进程继续执行，尝试打印，因为 cprintf 需要在栈上建立缓冲区存放打印文本，所以涉及到对栈的写操作，触发 page fault，可以看到进程2（也就是父进程）触发写的 page fault，之后输出`COW handled by copying page!`，这是在有人共享的物理页上触发复制时的打印，符合当前和子进程共享的状态。

而后进程3（也就是子进程）也触发写 page fault，因为它也使用了cprintf，也需要建立缓冲区，并且位置刚好和父进程相同，而那处物理页的内容因为父进程已经复制了一份，所以原来的物理页只有子进程在使用，所以原地更新，输出日志`COW optimized for refcount 1!`，这是在对只有一个进程使用的物理页转成可写物理页时的日志。最后，exit pass。

说明我们目前的COW成功处理了子进程的物理页分配，并且引用计数管理也是正确的。

#### dirty cow

**模拟**：如果在 do_pgfault 处理 COW 的过程中（比如 memcpy 很慢的时候），发生了进程切换，另一个线程（如果 ucore 支持多线程）修改了该页面的引用计数或者映射关系，就可能导致逻辑错误。

**解决方案**：核心是原子性（Atomicity）。在处理 COW 的整个过程中（检查-拷贝-替换），必须持有锁（比如 mm->page_table_lock），保证在这个期间，没有其他线程能修改页表结构或引用计数。

### challenge2 ：说明该用户程序是何时被预先加载到内存中的？与我们常用操作系统的加载有何区别，原因是什么？
在proc.c中：
```cpp
static int
init_main(void *arg)
{
    size_t nr_free_pages_store = nr_free_pages();
    size_t kernel_allocated_store = kallocated();

    //user_main 才被“加载”到内核栈中等待执行
    int pid = kernel_thread(user_main, NULL, 0);
    if (pid <= 0)
    {
        panic("create user_main failed.\n");
    }

    while (do_wait(0, NULL) == 0)
    {
        schedule();
    }

    cprintf("all user-mode processes have quit.\n");
    assert(initproc->cptr == NULL && initproc->yptr == NULL && initproc->optr == NULL);
    assert(nr_process == 2);
    assert(list_next(&proc_list) == &(initproc->list_link));
    assert(list_prev(&proc_list) == &(initproc->list_link));

    cprintf("init check memory pass.\n");
    return 0;
}
```
也就是说，从开机的时序图为:
```cpp
系统启动
  └─ proc_init()
        ├─ alloc idleproc
        └─ kernel_thread(init_main)
                └─ do_fork()
                        └─ 生成 init_main 内核线程
  init_main 运行
      └─ kernel_thread(user_main)
            └─ do_fork() 生成 user_main 内核线程
                  └─ user_main 内核线程运行
                        └─ KERNEL_EXECVE(exit) → do_execve → load_icode()
                              └─ 用户程序 ELF 被加载到内存
```
与常用操作系统的加载的区别：
ucore设计上省去了复杂的启动程序、文件系统和内存管理，直接用内核线程做“用户程序”的加载和启动。
原因是ucore没有完整文件系统和初始化脚本，所有用户程序都是编译进内核或静态二进制，通过内核线程直接 fork/load
而Linux/Windows 依赖文件系统和驱动支持，因此用户程序是在运行时按需加载，启动顺序更灵活。

### 分支任务一：GDB 调试 QEMU 页表查询过程

本次实验旨在通过双重 GDB 调试架构，深入 QEMU 源码内部，观察软件是如何模拟硬件 MMU 进行虚拟地址到物理地址转换的。实验首先需要重新编译带有调试信息的 QEMU 4.1.1，并配置 ucore 的 Makefile 以使用该版本。在调试过程中，我们通过在 ucore 端控制内核执行流程，并在 QEMU 端对核心翻译函数 get_physical_address 设置断点，成功捕捉到了地址翻译的现场。

在调试初期，我们捕捉到了一个非常有趣的现象。当我们在 ucore GDB 中对内核入口地址 0xffffffffc02000d6 设置断点时，QEMU 端的断点被立刻触发。通过分析调用栈（Backtrace），我们发现这次调用并非源自 CPU 的正常访存指令（即非 tlb_fill 路径），而是源自 gdb_breakpoint_insert 调用了 riscv_cpu_get_phys_page_debug。这揭示了 QEMU 的设计细节：模拟器复用了地址翻译逻辑来服务外部调试器，为了在正确的物理地址插入指令断点，QEMU 必须先将我们在 GDB 中指定的虚拟地址转换为物理地址。
```bash
158     {
(gdb) n
163         int mode = mmu_idx;
(gdb) 
165         if (mode == PRV_M && access_type != MMU_INST_FETCH) {
(gdb) 
171         if (mode == PRV_M || !riscv_feature(env, RISCV_FEATURE_MMU)) {
(gdb) 
177         *prot = 0;
(gdb) 
181         int mxr = get_field(env->mstatus, MSTATUS_MXR);
(gdb) 
183         if (env->priv_ver >= PRIV_VERSION_1_10_0) {
(gdb) 
184             base = get_field(env->satp, SATP_PPN) << PGSHIFT;
(gdb) 
185             sum = get_field(env->mstatus, MSTATUS_SUM);
(gdb) 
186             vm = get_field(env->satp, SATP_MODE);
(gdb) 
187             switch (vm) {
(gdb) 
191               levels = 3; ptidxbits = 9; ptesize = 8; break;
(gdb) 
223         CPUState *cs = env_cpu(env);
(gdb) 
224         int va_bits = PGSHIFT + levels * ptidxbits;
(gdb) 
225         target_ulong mask = (1L << (TARGET_LONG_BITS - (va_bits - 1))) - 1;
(gdb) 
226         target_ulong masked_msbs = (addr >> (va_bits - 1)) & mask;
(gdb) 
227         if (masked_msbs != 0 && masked_msbs != mask) {
(gdb) 
231         int ptshift = (levels - 1) * ptidxbits;
(gdb) 
237         for (i = 0; i < levels; i++, ptshift -= ptidxbits) {
(gdb) 
238             target_ulong idx = (addr >> (PGSHIFT + ptshift)) &
(gdb) 
239                                ((1 << ptidxbits) - 1);
(gdb) 
238             target_ulong idx = (addr >> (PGSHIFT + ptshift)) &
(gdb) 
242             target_ulong pte_addr = base + idx * ptesize;
(gdb) 
244             if (riscv_feature(env, RISCV_FEATURE_PMP) &&
(gdb) 
245                 !pmp_hart_has_privs(env, pte_addr, sizeof(target_ulong),
(gdb) 
244             if (riscv_feature(env, RISCV_FEATURE_PMP) &&
(gdb) 
252             target_ulong pte = ldq_phys(cs->as, pte_addr);
(gdb) 
254             target_ulong ppn = pte >> PTE_PPN_SHIFT;
(gdb) 
256             if (!(pte & PTE_V)) {
(gdb) 
259             } else if (!(pte & (PTE_R | PTE_W | PTE_X))) {
(gdb) 
262             } else if ((pte & (PTE_R | PTE_W | PTE_X)) == PTE_W) {
(gdb) 
265             } else if ((pte & (PTE_R | PTE_W | PTE_X)) == (PTE_W | PTE_X)) {
(gdb) 
268             } else if ((pte & PTE_U) && ((mode != PRV_U) &&
(gdb) 
273             } else if (!(pte & PTE_U) && (mode != PRV_S)) {
(gdb) 
276             } else if (ppn & ((1ULL << ptshift) - 1)) {
(gdb) 
279             } else if (access_type == MMU_DATA_LOAD && !((pte & PTE_R) ||
(gdb) 
283             } else if (access_type == MMU_DATA_STORE && !(pte & PTE_W)) {
(gdb) 
286             } else if (access_type == MMU_INST_FETCH && !(pte & PTE_X)) {
(gdb) 
292                     (access_type == MMU_DATA_STORE ? PTE_D : 0);
(gdb) 
291                 target_ulong updated_pte = pte | PTE_A |
(gdb) 
295                 if (updated_pte != pte) {
(gdb) 
333                 target_ulong vpn = addr >> PGSHIFT;
(gdb) 
334                 *physical = (ppn | (vpn & ((1L << ptshift) - 1))) << PGSHIFT;
(gdb) 
337                 if ((pte & PTE_R) || ((pte & PTE_X) && mxr)) {
(gdb) 
338                     *prot |= PAGE_READ;
(gdb) 
340                 if ((pte & PTE_X)) {
(gdb) 
341                     *prot |= PAGE_EXEC;
(gdb) 
345                 if ((pte & PTE_W) &&
(gdb) 
346                         (access_type == MMU_DATA_STORE || (pte & PTE_D))) {
(gdb) 
347                     *prot |= PAGE_WRITE;
(gdb) 
349                 return TRANSLATE_SUCCESS;
(gdb) 
353     }
```

随后，我们观察了真实的指令访存翻译流程。当 CPU 试图访问内核代码段地址 0xffffffffc02000d6 且 TLB 未命中时，最终调用了 target/riscv/cpu_helper.c 中的 get_physical_address 函数。代码首先通过读取 env->satp 寄存器获取了根页表的物理基地址：base = get_field(env->satp, SATP_PPN) << PGSHIFT;。随后，代码进入了一个设计为 3 次迭代的循环 for (i = 0; i < levels; ...)，用于模拟 Sv39 架构的三级页表遍历。

在循环内部，QEMU 通过 ldq_phys 函数模拟了硬件读取物理内存中页表项（PTE）的行为。关键的发现出现在循环的第一次迭代（Level 2 根页表）中：我们观察到读取出的 PTE 此时已经具备了读（R）和执行（X）权限。代码执行了如下判断：else if ((pte & (PTE_R | PTE_W | PTE_X)) == (PTE_W | PTE_X))。由于条件成立，循环提前终止，代码直接利用当前 PTE 中的物理页号（PPN）与虚拟地址的低位拼接计算出了最终物理地址。这一现象证实了 ucore 内核在映射其高地址空间时，使用了 1GB 大小的巨型页（Gigapage），这是一种为了减少 TLB Miss 和页表查询次数的常见优化手段。

此外，在观察 memset 等频繁访存操作时，我们注意到 QEMU 的断点并未频繁触发。这是因为 QEMU 实现了软件 TLB 机制（位于 accel/tcg/cputlb.c），只有当 TLB 查找失败（Miss）时，才会调用 riscv_cpu_tlb_fill 并最终进入我们调试的 C 语言翻译函数。这从侧面印证了模拟器通过 TLB 缓存来加速地址翻译的逻辑与真实硬件是一致的。

### 分支任务二：GDB 调试系统调用与特权级切换

本次实验的目的是追踪 RISC-V 架构下从用户态（User Mode）陷入内核态（Supervisor Mode）处理系统调用，并最终返回的完整硬件模拟过程。我们利用 add-symbol-file 加载了用户程序 exit 的符号表，并定位到了 user/libs/syscall.c 中内联汇编的 ecall 指令位置，以此作为切入点进行观测。

当用户程序执行 ecall 指令时，QEMU 端的控制流进入了 target/riscv/cpu_helper.c 中的 riscv_cpu_do_interrupt 函数。通过 GDB 单步调试，我们清晰地看到了 QEMU 模拟硬件处理异常的逻辑。首先，代码通过 cs->exception_index 识别出当前的异常原因（Cause）为 RISCV_EXCP_U_ECALL（用户态系统调用），并检查 deleg 寄存器确认该异常已被委托给 Supervisor 模式处理。

随后，代码执行了保存硬件上下文的关键操作。
```bash
507         RISCVCPU *cpu = RISCV_CPU(cs);
(gdb) n
508         CPURISCVState *env = &cpu->env;
(gdb) 
513         bool async = !!(cs->exception_index & RISCV_EXCP_INT_FLAG);
(gdb) 
514         target_ulong cause = cs->exception_index & RISCV_EXCP_INT_MASK;
(gdb) 
515         target_ulong deleg = async ? env->mideleg : env->medeleg;
(gdb) 
516         target_ulong tval = 0;
(gdb) 
525         if (!async) {
(gdb) 
527             switch (cause) {
(gdb) 
540                 break;
(gdb) 
543             if (cause == RISCV_EXCP_U_ECALL) {
(gdb) 
544                 assert(env->priv <= 3);
(gdb) 
545                 cause = ecall_cause_map[env->priv];
(gdb) 
549         trace_riscv_trap(env->mhartid, async, cause, env->pc, tval, cause < 16 ?
(gdb) 
550             (async ? riscv_intr_names : riscv_excp_names)[cause] : "(unknown)");
(gdb) 
549         trace_riscv_trap(env->mhartid, async, cause, env->pc, tval, cause < 16 ?
(gdb) 
552         if (env->priv <= PRV_S &&
(gdb) 
553                 cause < TARGET_LONG_BITS && ((deleg >> cause) & 1)) {
(gdb) 
555             target_ulong s = env->mstatus;
(gdb) 
556             s = set_field(s, MSTATUS_SPIE, env->priv_ver >= PRIV_VERSION_1_10_0 ?
(gdb) 
558             s = set_field(s, MSTATUS_SPP, env->priv);
```

我们观察到 QEMU 更新了状态寄存器 sstatus：s = set_field(s, MSTATUS_SPP, env->priv);，这一行代码将当前的特权级（User Mode）记录在 SPP 位中，为后续的返回做准备。同时，当前指令的地址（即 ecall 的地址）被保存到了 sepc 寄存器中：env->sepc = env->pc;。最关键的控制流转移发生在对 PC 指针的修改上：env->pc = (env->stvec >> 2 << 2) ...，这模拟了硬件跳转到 stvec 指定的陷阱向量表基址（即内核的 __alltraps 入口）。最后，riscv_cpu_set_mode(env, PRV_S) 被调用，标志着 CPU 正式完成了特权级的提升。

系统调用处理完毕后，内核执行 sret 指令返回用户态。我们在 QEMU 中拦截了处理该指令的辅助函数 helper_sret。调试显示，该函数执行了与中断进入相反的操作。它首先读取 sstatus 中的 SPP 位，确认之前的特权级是 User Mode。接着，它将 sepc 寄存器中保存的地址赋值回 PC：env->pc = retpc;，从而将控制流恢复到 ecall 指令的下一条指令。整个调试过程直观地展示了软硬件协同工作的细节，特别是 QEMU 如何通过 C 语言代码精确模拟 RISC-V 处理器在特权级切换时的寄存器操作和状态流转。