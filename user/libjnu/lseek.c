#include <jnu_syscall.h>

int64_t lseek(int fd, int64_t off, int whence)
{
	return (int64_t)jnu_syscall3(JNU_SYS_lseek, fd, off, whence);
}
