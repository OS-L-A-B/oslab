---
title: lab8：文件系统
author: 钱俊玮 朱荟宇 邹博闻
---
# <center>lab8：文件系统</center>

#### <center> 钱俊玮&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;朱荟宇&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;邹博闻 </center>
#### <center> 2312480&nbsp;2311824&nbsp;2312251 </center>

# lab8 练习1：完成读文件操作的实现

练习1要求完成 sfs_io_nolock 函数的实现，实现过程中的主要问题在处理边界对齐，因为用户请求的读写位置和大小显然不会一直完美，总会出现不会对齐到块边界的情况。

LAB8 需要实现的部分从计算起始块号和块数开始。blkno 表示读写操作的起始块号，通过 offset / SFS_BLKSIZE 得到；nblks 表示需要读写的完整块数，通过 endpos / SFS_BLKSIZE - blkno 计算得出。

实现的主要思路就是分三段处理。首先处理第一个块的非对齐部分。如果 offset 不是块边界对齐的（blkoff = offset % SFS_BLKSIZE 不为零），那么第一个块只需要读写从 blkoff 开始的部分。读写的大小取决于是否还有后续的完整块：
如果 nblks 不为零，说明后面还有完整块，那么第一个块读到末尾即可，大小为 SFS_BLKSIZE - blkoff；如果 nblks 为零，说明整个读写操作都在这一个块内，那么大小就是 endpos - offset。

```cpp
if ((blkoff = offset % SFS_BLKSIZE) != 0)
{
    size = (nblks != 0) ? (SFS_BLKSIZE - blkoff) : (endpos - offset);
    if ((ret = sfs_bmap_load_nolock(sfs, sin, blkno, &ino)) != 0)
    {
        goto out;
    }
    if ((ret = sfs_buf_op(sfs, buf, size, ino, blkoff)) != 0)
    {
        goto out;
    }
    alen += size;
    if (nblks == 0)
    {
        goto out;
    }
    buf += size;
    blkno++;
    nblks--;
}
```

sfs_bmap_load_nolock 负责将文件内的逻辑块号 blkno 转换为磁盘上的物理块号 ino，sfs_buf_op 执行实际的读写操作，它是一个函数指针，在函数开头根据 write 参数被设置为 sfs_rbuf 或 sfs_wbuf。处理完第一个块后，自然需要更新缓冲区指针、块号和剩余块数。

在处理完开头块后，接下来需要处理中间的完整块。这些块的读写都是从块的起始位置开始，大小都是 SFS_BLKSIZE，因此可以用 sfs_block_op 批量处理，效率比 sfs_buf_op 更高。循环处理每一个完整块，每次处理一个块就更新相关变量。

```cpp
while (nblks > 0)
{
    if ((ret = sfs_bmap_load_nolock(sfs, sin, blkno, &ino)) != 0)
    {
        goto out;
    }
    if ((ret = sfs_block_op(sfs, buf, ino, 1)) != 0)
    {
        goto out;
    }
    alen += SFS_BLKSIZE;
    buf += SFS_BLKSIZE;
    blkno++;
    nblks--;
}
```

最后处理尾部的非对齐块。如果 endpos 不是块边界对齐的，即 endpos % SFS_BLKSIZE 不为零，那么需要读写最后一个块的前一部分。这次是从块的起始位置开始读写，大小为 endpos % SFS_BLKSIZE。

```cpp
if ((size = endpos % SFS_BLKSIZE) != 0)
{
    if ((ret = sfs_bmap_load_nolock(sfs, sin, blkno, &ino)) != 0)
    {
        goto out;
    }
    if ((ret = sfs_buf_op(sfs, buf, size, ino, 0)) != 0)
    {
        goto out;
    }
    alen += size;
}
```

无论读写区域如何对齐，都必然可以分为首块的部分内容、中间若干完整块、尾块的部分内容这三段（更理想的情况自然是对齐）。分成三段处理保证了处理的正确性，同时对完整块使用 sfs_block_op 提高了效率。函数的最后在 out 标签处统一处理返回，更新文件大小并设置脏位标记。

# lab8 练习2：完成基于文件系统的执行程序机制的实现

练习2要求修改 load_icode 函数，使其能够从文件系统加载可执行文件，而不是像 Lab5 那样从内存中的二进制数据加载。这个改动的核心在于将直接的内存访问替换为通过文件描述符的文件读取操作，同时还需要支持命令行参数的传递。

函数的参数从原来的 binary 和 size 改为了 fd、argc 和 kargv。fd 是文件描述符，指向要加载的可执行文件；argc 和 kargv 则是命令行参数的个数和内容，需要传递给用户程序。函数的前半部分创建新的内存管理结构和页表的逻辑保持不变，主要的修改集中在如何读取 ELF 文件内容和如何构建用户栈。

读取 ELF 文件头的方式发生了变化。在 Lab5 中，可以直接通过指针访问内存中的数据，但现在需要通过 load_icode_read 函数从文件中读取。这个函数封装了 sysfile_read 调用，能够从文件的指定偏移位置读取指定长度的数据到缓冲区。读取 ELF 头时，从文件偏移 0 处读取 sizeof(struct elfhdr) 字节到 elf 指向的缓冲区。

```cpp
struct elfhdr __elf, *elf = &__elf;
if ((ret = load_icode_read(fd, elf, sizeof(struct elfhdr), 0)) != 0)
{
    goto bad_pgdir_cleanup_mm;
}
```

读取程序段头的过程类似。ELF 头中的 e_phnum 字段指明了程序段的数量，e_phoff 字段指明了程序段表在文件中的偏移。对于每个程序段，需要计算其段头在文件中的位置 phoff = elf->e_phoff + sizeof(struct proghdr) * ph_idx，然后用 load_icode_read 读取。

接下来是加载程序段数据部分。对于类型为 ELF_PT_LOAD 的段，需要将其内容从文件加载到内存中。首先根据段的标志位设置权限，然后用 mm_map 建立虚拟内存区域。加载数据时需要逐页处理，因为虚拟地址可能不是页对齐的。对于每一页，先用 pgdir_alloc_page 分配物理页并建立映射，然后计算该页内需要加载数据的位置和大小。第一页可能只需要加载部分内容（从 start - la 的偏移开始），最后一页也可能只加载部分内容，中间的页则全部加载。

```cpp
while (start < end)
{
    struct Page *page = pgdir_alloc_page(mm->pgdir, la, perm);
    if (page == NULL)
    {
        ret = -E_NO_MEM;
        goto bad_pgdir_cleanup_mm;
    }
    size_t off = start - la , size = PGSIZE - off;
    la += PGSIZE;
    if (end < la)
    {
        size -= la - end;
    }
    if ((ret = load_icode_read(fd, page2kva(page) + off, size, offset)) != 0)
    {
        goto bad_pgdir_cleanup_mm;
    }
    start += size;
    offset += size;
}
```

程序段的 p_memsz 字段通常大于 p_filesz 字段，差值部分就是 BSS 段，即未初始化的全局变量区域。这部分内存需要分配并清零，但不需要从文件读取。

处理 BSS 段时也要注意页对齐的问题，如果 BSS 段和前面的数据段共享同一页的后半部分，需要先对该页的剩余部分清零，然后再为 BSS 段剩余部分分配新的页面并全部清零。

用户栈的建立包括两个部分：首先是用 mm_map 建立用户栈的虚拟内存区域，然后用 pgdir_alloc_page 为栈分配物理页。Lab8 需要新增的就是在用户栈上构建命令行参数。参数的布局遵循标准的 C 语言 main 函数调用约定。从用户栈顶向下，首先预留 argv 指针数组的空间，然后逐个复制参数字符串。每复制一个字符串，就在 argv 数组中记录其地址。最后，在 argv 数组下方压入 argc 的值。这样当用户程序的 main 函数被调用时，就能正确地访问到 argc 和 argv。

```cpp
uintptr_t stacktop = USTACKTOP;
char **uargv = (char **)(stacktop - argc * sizeof(char *));
stacktop = (uintptr_t)uargv;

for (ph_idx = 0; ph_idx < argc; ph_idx++)
{
    size_t len = strlen(kargv[ph_idx]) + 1;
    stacktop -= len;
    uargv[ph_idx] = (char *)stacktop;
    memcpy((void *)stacktop, kargv[ph_idx], len);
}

stacktop = (uintptr_t)uargv - sizeof(int);
*(int *)stacktop = argc;
```

最后是设置 trapframe，使得从内核返回用户态时能正确跳转到用户程序入口并开始执行。tf->epc 置为 ELF 文件的入口地址 elf->e_entry，tf->gpr.sp 设置为刚才构建好的栈顶位置，即 argc 的地址。tf->status 需要设置 SSTATUS_SPIE 位以允许中断，同时清除 SSTATUS_SPP 位以确保 sret 指令返回用户态而不是内核态。

```cpp
struct trapframe *tf = current->tf;
memset(tf, 0, sizeof(struct trapframe));
tf->epc = elf->e_entry;
tf->gpr.sp = stacktop;
tf->status = (read_csr(sstatus) | SSTATUS_SPIE) & ~SSTATUS_SPP;
```
# lab8 Challenge1：UNIX 管道机制设计方案

## 数据结构（示例）
```c
// 单个管道缓冲块的描述
struct pipe_buffer {
    char *data;              // 指向环形缓冲区的起始位置
    size_t len;              // 当前有效数据长度
};

// 管道内核对象（对应 Linux 的 pipe_inode_info）
struct pipe_inode_info {
    char *buf;               // 实际的环形缓冲区
    size_t size;             // 缓冲区总长度，典型 4K 或页大小
    size_t head, tail;       // 读写指针（head 读，tail 写），在 size 上取模
    int readers, writers;    // 打开该管道的读/写端引用计数
    semaphore_t lock;        // 互斥访问缓冲区的锁
    wait_queue_t rq;         // 读等待队列，缓冲区为空时睡眠
    wait_queue_t wq;         // 写等待队列，缓冲区满时睡眠
};

// 文件表项保持对管道的引用
struct file {
    struct pipe_inode_info *pipe; // 如果是管道文件，则指向管道对象
    int flags;                    // O_RDONLY / O_WRONLY 等
    off_t pos;                    // 对管道通常忽略
    atomic_int refcnt;            // 文件描述符引用计数
};
```

## 关键接口与语义
- `int sys_pipe(int fd[2])`：创建 `pipe_inode_info`，初始化缓冲区/锁/等待队列，分配两个文件表项（读端、写端），返回文件描述符。
- `ssize_t pipe_read(struct file *f, char *buf, size_t n)`：若缓冲区为空且存在写端则睡眠在 `rq`；若写端已全部关闭则返回 0（EOF）；否则从环形缓冲区搬运数据，唤醒写等待队列。
- `ssize_t pipe_write(struct file *f, const char *buf, size_t n)`：若缓冲区满且有读端则睡眠在 `wq`；若读端都关闭则返回 `-EPIPE` 并向写进程发送 `SIGPIPE`；写入后唤醒读等待队列。
- `int pipe_release(struct file *f)`：减少对应 `readers` 或 `writers`，当任一端计数归零时唤醒对端队列处理 EOF/错误；当两端都为 0 时释放管道对象。
- `int pipe_poll(struct file *f, poll_table *p)`：支持 `select/poll`，根据缓冲区是否为空/满返回可读/可写事件。
- `int pipe_ioctl(struct file *f, unsigned cmd, unsigned long arg)`：可支持获取缓冲区大小、当前可读/可写空间等。

## 同步与互斥处理
- 所有读写对 `head/tail` 的修改需在 `lock` 保护下，防止并发破坏环形缓冲区。
- 读空/写满时使用 `rq/wq` 阻塞并在对端释放空间/关闭端点时唤醒，避免忙等。
- 关闭语义：写端关闭后，读端读空返回 0；读端关闭后，写端返回 `-EPIPE` 并触发 `SIGPIPE`。
- 引用计数确保管道对象在最后一个端关闭前不被释放；配合文件表项 `refcnt` 避免竞态释放。
- 若需要跨 CPU，多核环境下等待队列和计数器采用原子操作或禁中断自旋锁，保证顺序一致性。

---

# lab8 Challenge2：UNIX 软链接与硬链接机制设计方案

## 数据结构（示例）
```c
// 目录项（Dentry），用来从名字到 inode 的映射
struct dentry {
    char name[MAX_NAME_LEN];
    struct inode *inode;         // 指向目标 inode
    struct dentry *parent;
    struct list_head children;   // 子目录项链表
    atomic_int refcnt;
    semaphore_t lock;            // 目录项级别的互斥
};

// 文件 inode 扩展字段
struct inode {
    uint32_t ino;                // 唯一编号
    umode_t mode;                // 文件类型与权限（含 S_IFLNK）
    uint32_t nlink;              // 硬链接计数
    struct super_block *sb;
    struct inode_ops *i_op;      // 与类型相关的操作表
    struct file_ops *f_op;
    semaphore_t i_lock;          // inode 互斥锁
    union {
        // 普通文件/目录使用的块映射信息
        struct block_map mapping;
        // 软链接的内容（快速路径：若长度 < 阈值可内嵌）
        struct {
            char target[INLINE_SYMLINK]; // 软链接目标路径
            size_t len;
        } symlink;
    };
};
```

## 关键接口与语义
- `int sys_link(const char *old, const char *new)`：解析 `old` 得到目标 inode，递增 `nlink`；在 `new` 所在目录创建指向该 inode 的新目录项。禁止跨设备和对目录创建硬链接（除非特权）。
- `int sys_unlink(const char *path)`：删除目录项，递减 inode `nlink`；当 `nlink==0` 且无打开文件引用时回收数据块和 inode。
- `int sys_symlink(const char *target, const char *linkpath)`：创建类型为 `S_IFLNK` 的新 inode，将 `target` 字符串写入 symlink 数据区，在父目录加入目录项。
- `ssize_t sys_readlink(const char *path, char *buf, size_t bufsz)`：读取符号链接内容但不跟随它，返回目标路径长度。
- `int namei(const char *path, int follow_link)`：路径解析；若 `follow_link` 为真则在遇到 `S_IFLNK` 时展开目标路径（带上跳转深度上限以防循环），否则返回链接 inode 本身。
- `int rename(const char *old, const char *new)`：在同一挂载/文件系统内移动目录项，维护引用计数和父目录锁。

## 同步与互斥处理
- 目录操作需要按从父到子顺序持锁，避免死锁；目录项锁保护同一目录下的创建/删除/重命名并发。
- inode 的 `i_lock` 序列化对 `nlink` 和元数据的修改，配合原子 `refcnt` 防止回收竞态。
- 路径解析时，遇到符号链接需在持锁的同时检查循环计数（如 40 层上限），避免无限展开。
- `unlink`/`link`/`symlink` 与打开文件并发时，通过 `nlink` 和打开计数协同：数据仅在两者都为 0 时释放。
- 软链接的读取和展开在只读场景可使用读写锁或 RCU（若已有）减小竞争；写入链接目标仅在创建/重命名阶段，加独占锁即可。

以上设计在 ucore 中可复用现有 VFS 层（inode、dentry、superblock、file）框架，只需补充管道/链接类型的 inode、操作表和阻塞同步语义即可。