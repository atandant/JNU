#include <jnu_syscall.h>

int spawn(const char *path, char *const argv[])
{
	return (int)jnu_syscall2(JNU_SYS_spawn, (long)path, (long)argv);
}
