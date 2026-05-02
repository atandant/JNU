#include <jnu_syscall.h>

int main(int argc, char **argv)
{
	static const char msg[] = "JNU hello: child process alive argv0=";
	const char *arg0 = argc > 0 ? argv[0] : "(none)";
	size_t len = 0;

	(void)write(1, msg, sizeof(msg) - 1);
	while (arg0[len]) {
		len++;
	}
	(void)write(1, arg0, len);
	(void)write(1, "\n", 1);
	return 7;
}
