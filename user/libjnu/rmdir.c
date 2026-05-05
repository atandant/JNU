#include <jnu_syscall.h>

int rmdir(const char *path)
{
	return (int)jnu_syscall1(JNU_SYS_rmdir, (long)path);
}
