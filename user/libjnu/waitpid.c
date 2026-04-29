#include <jnu_syscall.h>

int waitpid(int pid, int *status)
{
	return (int)jnu_syscall2(JNU_SYS_waitpid, pid, (long)status);
}
