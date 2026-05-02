#include <jnu_syscall.h>

int execve(const char *path, char *const argv[], char *const envp[])
{
	return (int)jnu_syscall3(JNU_SYS_execve, (long)path, (long)argv,
				 (long)envp);
}
