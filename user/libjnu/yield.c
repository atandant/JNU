#include <jnu_syscall.h>

int yield(void)
{
	return (int)jnu_syscall0(JNU_SYS_yield);
}
