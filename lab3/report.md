---
title: lab3：中断与中断处理流程
author: 钱俊玮 朱荟宇 邹博闻
---
# <center>lab3:中断与中断处理流程</center>

#### <center> 钱俊玮&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;朱荟宇&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;邹博闻 </center>
#### <center> 2312480&nbsp;2311824&nbsp;2312251 </center>

### exercise 1：编程完善中断处理

编程完善trap.c中的中断处理函数trap，在对时钟中断进行处理的部分填写kern/trap/trap.c函数中处理时钟中断的部分，使操作系统每遇到100次时钟中断后，调用print_ticks子程序，向屏幕上打印一行文字”100 ticks”，在打印完10行后调用sbi.h中的shut_down()函数关机。

#### 代码分析

本次任务代码编写难度较低，完善中断处理函数中的IRQ_S_TIMER case即可：
```cpp
    case IRQ_S_TIMER:
        /* 设置下次时钟中断 */
        clock_set_next_event();

        /* 静态变量用于在多次中断中保存计数 */
        static int print_count = 0;

        /* 计数器加一 */
        ticks++;

        /* 当计数到达 TICK_NUM 时打印并统计打印次数 */
        if (ticks % TICK_NUM == 0) {
            print_ticks();        /* 打印 "100 ticks" */
            print_count++;        /* 打印次数加一 */

            /* 打印达到 10 次时关机 */
            if (print_count >= 10) {
                sbi_shutdown();
                /* sbi_shutdown() 一般不会返回；若返回，可在此处理 */
            }
        }
        break;
```
通过维护一个静态变量，就可以实现exercise 1要求的调用十次print_ticks后关机的目标。当然更重要的是对中断执行过程进行分析。
### challenge 1：描述中断发生及处理过程
#### init.c
init.c的主要任务主要有三个：
（1）调用idt_init()。这个函数内，write_csr(sscratch, 0)设置为内核态，write_csr(stvec, &alltraps)设置异常向量地址，之后任何中断/异常发生，CPU都会跳转到alltraps入口，而这个入口具体的作用需要参照trapentry.S。
trapentry.S中，SAVE_ALL部分和move a0,sp将在challenge 1解释，剩下的就是jal trap。
（2）调用clock_init()。clock_init()代码阅读难度也不是很高：
```cpp
    void clock_init(void)
    {
        set_csr(sie, MIP_STIP);
        clock_set_next_event();
        ticks = 0;

        cprintf("++ setup timer interrupts\n");
    }
    void clock_set_next_event(void) { sbi_set_timer(get_cycles() + timebase); }
```
sie寄存器负责控制哪些类型的中断可以被响应，MIP_STIP是0x20，即将sie的第五位置为1，而这一位就是负责S模式下的计时器中断使能。之后调用clock_set_next_event()，这个函数通过调用SBI提供的sbi_set_timer()实现计时器设置。设置后在硬件层面比较mtime和mtimecmp的情况，时间到后设置mip.STIP位（即中断等待标志）。
（3）调用intr_enable()。该函数内只有set_csr(sstatus, SSTATUS_SIE)，其中sstatus负责保存处理器的状态信息，SSTATUS_SIE=2，这与寄存器状况是对应的，sstatus寄存器的第二位是SIE，使能S态的全局中断。为0时S态的中断不发生；当运行在U态，SIE中的值被忽略，S态中断使能总被允许。

以上三个函数之间的关系：首先需要idt_init()确保处在内核态，并设置好异常向量地址，确定触发中断后的下一步；clock_init()设置sie寄存器可以响应定时器中断，并通过sbi提供的接口设置一个timer；intr_enable()设置启用中断。
#### 定时器中断触发过程
当计时器mtime=mtimecmp时，CPU自动将scause置为0x8000000000000005，并将mip.STIP置为1（mip是中断寄存器，STIP是S模式下的时钟中断对应位），此时CPU先检查sstatus.SIE，而这一位已经被intr_enable()置为1，全局中断启用，CPU继续在sie中检查中断具体类型。由于sie.STIP=1（由clock_init()设置），此时允许CPU响应S模式下的计时器中断，因此CPU响应中断，去stvec寄存器中获取异常向量入口地址，而这个寄存器已经在idt_init()中被设置好了，跳转到__alltraps。

#### __alltraps工作过程

当CPU跳转到__alltraps后，首先会执行保存现场的操作。该保护操作是通过SAVE_ALL宏实现的。该宏主要有三个任务：
（1）记录当前栈指针，并给保存现场预留空间。csrw sscratch, sp记录栈指针，sscratch用来保存中断或异常处理时的临时值。之后将栈顶抬高36字节。
（2）保存通用寄存器的值。通过32个STORE指令，保存x0到x31。xi寄存器被保存在sp+i* REGBYTES的位置。
（3）保存SCR寄存器。s0到s4分别存当前栈指针，sstatus情况，pc，异常相关地址stval，异常/中断原因scause。然后继续STORE存到栈中。

保存完成后，alltraps会将当前栈指针sp的值传递给a0寄存器，并通过jal trap跳转到C语言实现的trap()函数。此时，trap()函数会根据传入的trapframe结构体中的信息，进一步判断中断或异常的类型，并调用相应的处理函数。

需要指出的是，trapframe结构体中的变量布局与此处寄存器的保存顺序是对应的。寄存器保存时，sstatus、sepc、stval、scause存在靠近栈底的位置（32* REGBYTES~35* RERGBYTES），而struct trapframe中：
```cpp
    struct trapframe
    {
        struct pushregs gpr;
        uintptr_t status;
        uintptr_t epc;
        uintptr_t badvaddr;
        uintptr_t cause;
    };
```
其中pushregs是32个8B的int64，对应低地址的x0~x31，sstatus、epc、badvaddr、cause都与栈内位置一一对应。
#### 中断处理函数工作过程
trap函数接收传来的sp，调用trap_dispatch()，该函数内的if ((intptr_t)tf->cause < 0)可以判断是中断还是异常。如果是中断，由于最高位是1，则将其强转int后会变成一个负数。如果是异常则是整数。由于tf->cause=0x8000000000000005，所以进入进入中断处理函数interrupt_handler()。中断处理函数中，先左移后右移将最高位的1置0，并根据其他位的值判断中断类型。此处处理后值为5，进入IRQ_S_TIMER case。

现在的代码中，case内会再clock_set_next_event()设置一个新的计时器，也就是说每触发100次计时器中断后调用一次print_ticks()，调用10次打印后调用sbi_shutdown()。

#### 中断处理结束
jal trap调用结束后，trapentry.S继续向下执行，进入__trapret。该部分依次完成两件工作：
（1）RESTORE_ALL恢复现场。RESTORE_ALL通过一系列LOAD恢复中断发生时寄存器的值；
（2）sret。sret 是RISC-V中的一条特权指令，用于从S模式的中断或异常中返回到发生前的程序继续执行。将pc还原为sepc，并恢复sstatus的SIE位。



#### 回答几个问题

（1）mov a0，sp的目的？
通过以上对代码的分析，不难发现是将sp传入trap函数，sp指向栈顶，而紧靠栈顶的就是保存的现场，结构也与trapframe一一对应。将sp传入后，中断处理函数就可以拿到现场的寄存器信息了。

（2）对于任何中断，alltraps 中都需要保存所有寄存器吗？
前文已经比较详细地解释过trapframe的原理，显然此时保存所有寄存器更加通用。每次都保存所有寄存器，这样结构体才能与栈内信息一一对应，否则寄存器对应关系就会非常混乱。

### Challenge2：理解上下文切换机制

RISC-V 异常入口（trapentry.S）代码其实是操作系统级的“陷入入口保存现场”逻辑，里面的 csrw sscratch, sp 和 csrrw s0, sscratch, x0 这两句是整个异常现场保存的关键之一。我们一步步来拆解它们的含义与目的。

```cpp
csrw sscratch, sp
```
将当前内核栈指针 sp 写入 sscratch

目的：
当陷入（trap）发生时，硬件不会自动保存栈指针。而在陷入前，用户态和内核态的栈是不同的。
内核希望在 trap 入口时能使用一块安全的栈空间（通常是当前 CPU 的内核栈）。

于是，内核事先规定：

在进入内核（异常入口）前，sscratch 存放用户态的栈指针；
在内核处理 trap 时，sp 改为指向内核栈。

```cpp
csrrw s0, sscratch, x0
```
读取 sscratch 的值到 s0；

同时将寄存器 x0（恒为 0）写入 sscratch。

把用户态的 sp 暂存到 s0，等下要一并保存到陷入帧（方便将来 sret 返回用户态时恢复）。

把 sscratch 清零，是为了让之后如果又发生二次异常（nested trap），陷入入口通过检查 sscratch 是否为 0 来判断这次陷入是从内核态发生的（sscratch=0）还是从用户态发生的（sscratch≠0）。这样可以采用不同的异常处理路径，避免重复覆盖栈。

SAVE_ALL 保存这些 CSR 的原因是：

这些寄存器之后可能会被内核修改。

trap() 函数需要读取这些信息 来判断异常类型、打印错误、调度任务、恢复用户态等。

RESTORE_ALL 不恢复这些 CSR 的原因是：

这些 CSR 的值只在 trap 入口保存，用于内核分析异常。恢复用户态时，只需要恢复 sstatus 和 sepc。

sstatus 和 sepc 必须恢复：

sstatus 控制返回后的特权级和中断使能状态；
sepc 控制 sret 返回的目标地址（用户程序继续执行的位置）。

而 scause、stval（sbadaddr）只是异常信息，不影响 sret。
它们的值仅供异常处理函数 trap() 使用，返回用户态时不需要修改它们，也不应该还原，如果下次 trap 发生，硬件会重新设置它们。

### Challenge3：完善异常中断

在exercise 1里完成了时钟中断的处理，而这次实验需要在trap.c里异常处理函数中完成断点异常和非法指令异常的捕获和处理。

处理本身非常简单，按照指导书的内容，输出异常类型和触发异常的指令地址即可。
```cpp
case CAUSE_ILLEGAL_INSTRUCTION:
    cprintf("Exception type:Illegal instruction\n");
    cprintf("Illegal instruction caught at 0x%08x\n", tf->epc);
    tf->epc += 4; // 异常处理的下一条指令一般就是触发异常的指令的下一条指令地址
    break;
case CAUSE_BREAKPOINT:
    cprintf("Exception type: breakpoint\n");
    cprintf("ebreak caught at 0x%08x\n", tf->epc);
    tf->epc += 4;
    break;
```
触发异常的指令地址可以从trapframe结构体中获取，它保存了出现异常时的上下文，而结构体中的`epc`变量对应的就是sepc寄存器存储的地址。

稍微麻烦一点的是异常指令的产生。首先根据异常类型确定需要的异常指令：

+ 断点异常：`ebreak`。但因为可能存在C 扩展，导致将ebreak伪指令以16位地址进行解析，所以实际使用的是ebreak的32-bit硬编码：`.word 0x00100073`
+ 非法指令异常：`.word 0x00000000`

通过内联汇编将异常指令写入，并且读取相应位置的地址和异常处理的输出进行比较，确认异常处理正确性。异常触发代码如下：
```cpp
void __attribute__((noinline))
exception_trigger(void){
    uint32_t illegal_address,ebreak_address;
    asm volatile(
        ".option push\n"
        ".option norvc\n"          
        ".align 2\n"
        "   la %0, 1f\n"           // 先把 ebreak 的地址放进 %0（此时还没陷入）
        "   la %1, 2f\n"           // 再把 illegal 的地址放进 %1
        "1: .word 0x00100073\n"    // ebreak 的 32-bit 硬编码 (EBREAK)
        "2: .word 0x00000000\n"    // 非法指令：全 0
        ".option pop\n"
        : "=r"(ebreak_address),"=r"(illegal_address)
        :
        : "memory");
    cprintf("Expected ebreak address: 0x%08x\n", ebreak_address);
    cprintf("Expected illegal instruction address: 0x%08x\n", illegal_address);
}
```

将`exception_trigger`函数放置在init.c中初始化时`idt_init()`后面执行即可触发异常。相关输出如下：
```bash
Exception type: breakpoint
ebreak caught at 0xc020006c
Exception type:Illegal instruction
Illegal instruction caught at 0xc0200070
Expected ebreak address: 0xc020006c
Expected illegal instruction address: 0xc0200070
```