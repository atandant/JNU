#include <jnu_syscall.h>

int main(int argc, char **argv)
{
	static const char msg[] = "JNU hello: child process alive\n";

	(void)argc;
	(void)argv;
	(void)write(1, msg, sizeof(msg) - 1);
	return 7;
}
