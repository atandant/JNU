#include <jnu_syscall.h>

int fstat(int fd, void *st)
{
	return (int)jnu_syscall2(JNU_SYS_fstat, fd, (long)st);
}
