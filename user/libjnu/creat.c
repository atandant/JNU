#include <jnu_syscall.h>

int creat(const char *path, int mode)
{
	return (int)jnu_syscall2(JNU_SYS_creat, (long)path, mode);
}
