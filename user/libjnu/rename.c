#include <jnu_syscall.h>

int rename(const char *old_path, const char *new_path)
{
	return (int)jnu_syscall2(JNU_SYS_rename, (long)old_path,
				 (long)new_path);
}
