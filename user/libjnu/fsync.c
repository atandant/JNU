#include <jnu_syscall.h>

int fsync(int fd) { return (int)jnu_syscall1(JNU_SYS_fsync, fd); }
