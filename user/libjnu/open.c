#include <jnu_syscall.h>

int open(const char *path, int flags)
{
	return (int)jnu_syscall2(JNU_SYS_open, (long)path, flags);
}
