#include <jnu_syscall.h>

int unlink(const char *path)
{
	return (int)jnu_syscall1(JNU_SYS_unlink, (long)path);
}
