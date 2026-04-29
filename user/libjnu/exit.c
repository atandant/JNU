#include <jnu_syscall.h>

void exit(int status)
{
	(void)jnu_syscall1(JNU_SYS_exit, status);
	for (;;) {
		__asm__ __volatile__("hlt");
	}
}
