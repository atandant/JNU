#include <jnu_syscall.h>

int ftruncate(int fd, long length)
{
	return (int)jnu_syscall2(JNU_SYS_ftruncate, fd, length);
}
