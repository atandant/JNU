#include <jnu_syscall.h>

ssize_t write(int fd, const void *buf, size_t len)
{
	return (ssize_t)jnu_syscall3(JNU_SYS_write, fd, (long)buf, (long)len);
}
