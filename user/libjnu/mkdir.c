#include <jnu_syscall.h>

int mkdir(const char *path, int mode)
{
	return (int)jnu_syscall2(JNU_SYS_mkdir, (long)path, mode);
}
