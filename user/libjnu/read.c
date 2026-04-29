#include <jnu_syscall.h>

ssize_t read(int fd, void *buf, size_t len)
{
	return (ssize_t)jnu_syscall3(JNU_SYS_read, fd, (long)buf, (long)len);
}
