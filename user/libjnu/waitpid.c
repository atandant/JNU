#include <jnu_syscall.h>

/*
 * v0.0.3 §2.2: the underlying syscall is wait4(pid, status, options,
 * rusage). libjnu's waitpid() shim passes options=0 and rusage=NULL
 * — the kernel ignores both for now.
 */
int waitpid(int pid, int *status)
{
	return (int)jnu_syscall4(JNU_SYS_wait4, pid, (long)status, 0, 0);
}
