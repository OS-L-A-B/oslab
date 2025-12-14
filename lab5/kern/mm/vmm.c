#include <vmm.h>
#include <sync.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <error.h>
#include <pmm.h>
#include <riscv.h>
#include <kmalloc.h>

/*
  vmm design include two parts: mm_struct (mm) & vma_struct (vma)
  mm is the memory manager for the set of continuous virtual memory
  area which have the same PDT. vma is a continuous virtual memory area.
  There a linear link list for vma & a redblack link list for vma in mm.
---------------
  mm related functions:
   golbal functions
     struct mm_struct * mm_create(void)
     void mm_destroy(struct mm_struct *mm)
     int do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr)
--------------
  vma related functions:
   global functions
     struct vma_struct * vma_create (uintptr_t vm_start, uintptr_t vm_end,...)
     void insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma)
     struct vma_struct * find_vma(struct mm_struct *mm, uintptr_t addr)
   local functions
     inline void check_vma_overlap(struct vma_struct *prev, struct vma_struct *next)
---------------
   check correctness functions
     void check_vmm(void);
     void check_vma_struct(void);
     void check_pgfault(void);
*/

static void check_vmm(void);
static void check_vma_struct(void);

// mm_create -  alloc a mm_struct & initialize it.
struct mm_struct *
mm_create(void)
{
    struct mm_struct *mm = kmalloc(sizeof(struct mm_struct));

    if (mm != NULL)
    {
        list_init(&(mm->mmap_list));
        mm->mmap_cache = NULL;
        mm->pgdir = NULL;
        mm->map_count = 0;

        mm->sm_priv = NULL;

        set_mm_count(mm, 0);
        lock_init(&(mm->mm_lock));
    }
    return mm;
}

// vma_create - alloc a vma_struct & initialize it. (addr range: vm_start~vm_end)
struct vma_struct *
vma_create(uintptr_t vm_start, uintptr_t vm_end, uint32_t vm_flags)
{
    struct vma_struct *vma = kmalloc(sizeof(struct vma_struct));

    if (vma != NULL)
    {
        vma->vm_start = vm_start;
        vma->vm_end = vm_end;
        vma->vm_flags = vm_flags;
    }
    return vma;
}

// find_vma - find a vma  (vma->vm_start <= addr <= vma_vm_end)
struct vma_struct *
find_vma(struct mm_struct *mm, uintptr_t addr)
{
    struct vma_struct *vma = NULL;
    if (mm != NULL)
    {
        vma = mm->mmap_cache;
        if (!(vma != NULL && vma->vm_start <= addr && vma->vm_end > addr))
        {
            bool found = 0;
            list_entry_t *list = &(mm->mmap_list), *le = list;
            while ((le = list_next(le)) != list)
            {
                vma = le2vma(le, list_link);
                if (vma->vm_start <= addr && addr < vma->vm_end)
                {
                    found = 1;
                    break;
                }
            }
            if (!found)
            {
                vma = NULL;
            }
        }
        if (vma != NULL)
        {
            mm->mmap_cache = vma;
        }
    }
    return vma;
}

// check_vma_overlap - check if vma1 overlaps vma2 ?
static inline void
check_vma_overlap(struct vma_struct *prev, struct vma_struct *next)
{
    assert(prev->vm_start < prev->vm_end);
    assert(prev->vm_end <= next->vm_start);
    assert(next->vm_start < next->vm_end);
}

// insert_vma_struct -insert vma in mm's list link
void insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma)
{
    assert(vma->vm_start < vma->vm_end);
    list_entry_t *list = &(mm->mmap_list);
    list_entry_t *le_prev = list, *le_next;

    list_entry_t *le = list;
    while ((le = list_next(le)) != list)
    {
        struct vma_struct *mmap_prev = le2vma(le, list_link);
        if (mmap_prev->vm_start > vma->vm_start)
        {
            break;
        }
        le_prev = le;
    }

    le_next = list_next(le_prev);

    /* check overlap */
    if (le_prev != list)
    {
        check_vma_overlap(le2vma(le_prev, list_link), vma);
    }
    if (le_next != list)
    {
        check_vma_overlap(vma, le2vma(le_next, list_link));
    }

    vma->vm_mm = mm;
    list_add_after(le_prev, &(vma->list_link));

    mm->map_count++;
}

// mm_destroy - free mm and mm internal fields
void mm_destroy(struct mm_struct *mm)
{
    assert(mm_count(mm) == 0);

    list_entry_t *list = &(mm->mmap_list), *le;
    while ((le = list_next(list)) != list)
    {
        list_del(le);
        kfree(le2vma(le, list_link)); // kfree vma
    }
    kfree(mm); // kfree mm
    mm = NULL;
}

int mm_map(struct mm_struct *mm, uintptr_t addr, size_t len, uint32_t vm_flags,
           struct vma_struct **vma_store)
{
    uintptr_t start = ROUNDDOWN(addr, PGSIZE), end = ROUNDUP(addr + len, PGSIZE);
    if (!USER_ACCESS(start, end))
    {
        return -E_INVAL;
    }

    assert(mm != NULL);

    int ret = -E_INVAL;

    struct vma_struct *vma;
    if ((vma = find_vma(mm, start)) != NULL && end > vma->vm_start)
    {
        goto out;
    }
    ret = -E_NO_MEM;

    if ((vma = vma_create(start, end, vm_flags)) == NULL)
    {
        goto out;
    }
    insert_vma_struct(mm, vma);
    if (vma_store != NULL)
    {
        *vma_store = vma;
    }
    ret = 0;

out:
    return ret;
}

int dup_mmap(struct mm_struct *to, struct mm_struct *from)
{
    assert(to != NULL && from != NULL);
    list_entry_t *list = &(from->mmap_list), *le = list;
    while ((le = list_prev(le)) != list)
    {
        struct vma_struct *vma, *nvma;
        vma = le2vma(le, list_link);
        nvma = vma_create(vma->vm_start, vma->vm_end, vma->vm_flags);
        if (nvma == NULL)
        {
            return -E_NO_MEM;
        }

        insert_vma_struct(to, nvma);

        // bool share = 0;
        bool share = 1;
        if (copy_range(to->pgdir, from->pgdir, vma->vm_start, vma->vm_end, share) != 0)
        {
            return -E_NO_MEM;
        }
    }
    return 0;
}

void exit_mmap(struct mm_struct *mm)
{
    assert(mm != NULL && mm_count(mm) == 0);
    pde_t *pgdir = mm->pgdir;
    list_entry_t *list = &(mm->mmap_list), *le = list;
    while ((le = list_next(le)) != list)
    {
        struct vma_struct *vma = le2vma(le, list_link);
        unmap_range(pgdir, vma->vm_start, vma->vm_end);
    }
    while ((le = list_next(le)) != list)
    {
        struct vma_struct *vma = le2vma(le, list_link);
        exit_range(pgdir, vma->vm_start, vma->vm_end);
    }
}

bool copy_from_user(struct mm_struct *mm, void *dst, const void *src, size_t len, bool writable)
{
    if (!user_mem_check(mm, (uintptr_t)src, len, writable))
    {
        return 0;
    }
    memcpy(dst, src, len);
    return 1;
}

bool copy_to_user(struct mm_struct *mm, void *dst, const void *src, size_t len)
{
    if (!user_mem_check(mm, (uintptr_t)dst, len, 1))
    {
        return 0;
    }
    memcpy(dst, src, len);
    return 1;
}

// vmm_init - initialize virtual memory management
//          - now just call check_vmm to check correctness of vmm
void vmm_init(void)
{
    check_vmm();
}

// check_vmm - check correctness of vmm
static void
check_vmm(void)
{
    // size_t nr_free_pages_store = nr_free_pages();

    check_vma_struct();
    // check_pgfault();

    cprintf("check_vmm() succeeded.\n");
}

static void
check_vma_struct(void)
{
    // size_t nr_free_pages_store = nr_free_pages();

    struct mm_struct *mm = mm_create();
    assert(mm != NULL);

    int step1 = 10, step2 = step1 * 10;

    int i;
    for (i = step1; i >= 1; i--)
    {
        struct vma_struct *vma = vma_create(i * 5, i * 5 + 2, 0);
        assert(vma != NULL);
        insert_vma_struct(mm, vma);
    }

    for (i = step1 + 1; i <= step2; i++)
    {
        struct vma_struct *vma = vma_create(i * 5, i * 5 + 2, 0);
        assert(vma != NULL);
        insert_vma_struct(mm, vma);
    }

    list_entry_t *le = list_next(&(mm->mmap_list));

    for (i = 1; i <= step2; i++)
    {
        assert(le != &(mm->mmap_list));
        struct vma_struct *mmap = le2vma(le, list_link);
        assert(mmap->vm_start == i * 5 && mmap->vm_end == i * 5 + 2);
        le = list_next(le);
    }

    for (i = 5; i <= 5 * step2; i += 5)
    {
        struct vma_struct *vma1 = find_vma(mm, i);
        assert(vma1 != NULL);
        struct vma_struct *vma2 = find_vma(mm, i + 1);
        assert(vma2 != NULL);
        struct vma_struct *vma3 = find_vma(mm, i + 2);
        assert(vma3 == NULL);
        struct vma_struct *vma4 = find_vma(mm, i + 3);
        assert(vma4 == NULL);
        struct vma_struct *vma5 = find_vma(mm, i + 4);
        assert(vma5 == NULL);

        assert(vma1->vm_start == i && vma1->vm_end == i + 2);
        assert(vma2->vm_start == i && vma2->vm_end == i + 2);
    }

    for (i = 4; i >= 0; i--)
    {
        struct vma_struct *vma_below_5 = find_vma(mm, i);
        if (vma_below_5 != NULL)
        {
            cprintf("vma_below_5: i %x, start %x, end %x\n", i, vma_below_5->vm_start, vma_below_5->vm_end);
        }
        assert(vma_below_5 == NULL);
    }

    mm_destroy(mm);

    cprintf("check_vma_struct() succeeded!\n");
}
bool user_mem_check(struct mm_struct *mm, uintptr_t addr, size_t len, bool write)
{
    if (mm != NULL)
    {
        if (!USER_ACCESS(addr, addr + len))
        {
            return 0;
        }
        struct vma_struct *vma;
        uintptr_t start = addr, end = addr + len;
        while (start < end)
        {
            if ((vma = find_vma(mm, start)) == NULL || start < vma->vm_start)
            {
                return 0;
            }
            if (!(vma->vm_flags & ((write) ? VM_WRITE : VM_READ)))
            {
                return 0;
            }
            if (write && (vma->vm_flags & VM_STACK))
            {
                if (start < vma->vm_start + PGSIZE)
                { // check stack start & size
                    return 0;
                }
            }
            start = vma->vm_end;
        }
        return 1;
    }
    return KERN_ACCESS(addr, addr + len);
}

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
        
        // 咱们先停在这，等你消化完 Part 1，我们单独细写这一块。
        // 现在你可以先写个 cprintf("COW triggered!\n"); 占位。
        // 【插入这行】
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