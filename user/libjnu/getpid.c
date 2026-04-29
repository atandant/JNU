#include <jnu_syscall.h>

int getpid(void)
{
	return (int)jnu_syscall0(JNU_SYS_getpid);
}
