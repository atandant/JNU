#include <jnu_syscall.h>

#define O_RDONLY 0

static void puts(const char *s)
{
	size_t len = 0;

	while (s[len]) {
		len++;
	}
	(void)write(1, s, len);
}

static void put_uint(unsigned value)
{
	char buf[16];
	size_t i = sizeof(buf);

	if (value == 0) {
		puts("0");
		return;
	}

	while (value && i > 0) {
		buf[--i] = (char)('0' + value % 10);
		value /= 10;
	}
	(void)write(1, &buf[i], sizeof(buf) - i);
}

static char *const musltest_argv[] = {"musltest", 0};

static void kbd_echo_loop(void)
{
	int kbd_fd;
	char buf[64];
	ssize_t n;

	kbd_fd = open("/dev/kbd", O_RDONLY);
	if (kbd_fd < 0) {
		puts("JNU init: /dev/kbd open failed\n");
		return;
	}

	puts("JNU init: keyboard ready — type here (Ctrl+C not wired yet)\n");

	for (;;) {
		n = read(kbd_fd, buf, sizeof(buf));
		if (n > 0) {
			(void)write(1, buf, (size_t)n);
		}
		(void)yield();
	}
}

int main(int argc, char **argv)
{
	int status = 0;
	int child;

	(void)argc;
	(void)argv;

	puts("JNU init: hello from ring 3\n");
	puts("JNU init: pid ");
	put_uint((unsigned)getpid());
	puts("\n");

	child = fork();
	if (child == 0) {
		int err = execve("/bin/musltest", musltest_argv, 0);

		puts("JNU init: execve failed rc=");
		put_uint((unsigned)(-err));
		puts("\n");
		exit(127);
	}

	if (child > 0) {
		(void)waitpid(child, &status);
		puts("JNU init: musltest exited ");
		put_uint((unsigned)status);
		puts("\n");
	} else {
		puts("JNU init: fork failed\n");
	}

	kbd_echo_loop();
	return 0;
}
