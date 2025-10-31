#include <clock.h>
#include <console.h>
#include <defs.h>
#include <intr.h>
#include <kdebug.h>
#include <kmonitor.h>
#include <pmm.h>
#include <stdio.h>
#include <string.h>
#include <trap.h>
#include <dtb.h>

int kern_init(void) __attribute__((noreturn));
void grade_backtrace(void);
/*
 * 异常触发器，为了实现challenge3的非法指令异常和断点异常
*/
void __attribute__((noinline))
exception_trigger(void){
    uint32_t illegal_address,ebreak_address;
    asm volatile(
        ".option push\n"
        ".option norvc\n"          // 关 RVC：下面两条一定是 32-bit
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

int kern_init(void) {
    extern char edata[], end[];
    // 先清零 BSS，再读取并保存 DTB 的内存信息，避免被清零覆盖（为了解释变化 正式上传时我觉得应该删去这句话）
    memset(edata, 0, end - edata);
    dtb_init();
    cons_init();  // init the console
    const char *message = "(THU.CST) os is loading ...\0";
    //cprintf("%s\n\n", message);
    cputs(message);

    print_kerninfo();

    // grade_backtrace();
    idt_init();  // init interrupt descriptor table

    pmm_init();  // init physical memory management

    idt_init();  // init interrupt descriptor table
    exception_trigger(); // 触发异常

    clock_init();   // init clock interrupt
    intr_enable();  // enable irq interrupt

    /* do nothing */
    while (1)
        ;
}

void __attribute__((noinline))
grade_backtrace2(int arg0, int arg1, int arg2, int arg3) {
    mon_backtrace(0, NULL, NULL);
}

void __attribute__((noinline)) grade_backtrace1(int arg0, int arg1) {
    grade_backtrace2(arg0, (uintptr_t)&arg0, arg1, (uintptr_t)&arg1);
}

void __attribute__((noinline)) grade_backtrace0(int arg0, int arg1, int arg2) {
    grade_backtrace1(arg0, arg2);
}

void grade_backtrace(void) { grade_backtrace0(0, (uintptr_t)kern_init, 0xffff0000); }

