#include <jnu_syscall.h>

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

static void read_minix_file(void)
{
	char buf[80];
	struct jnu_stat st;
	ssize_t n;
	int fd;

	fd = open("/test.txt", 0);
	if (fd < 0) {
		puts("JNU init: /test.txt open failed\n");
		return;
	}

	if (fstat(fd, &st) == 0) {
		puts("JNU init: /test.txt size ");
		put_uint((unsigned)st.size);
		puts("\n");
	}

	n = read(fd, buf, sizeof(buf) - 1);
	if (n > 0) {
		buf[n] = '\0';
		puts("JNU init: /test.txt says: ");
		puts(buf);
		puts("\n");
	}

	(void)lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, 5);
	if (n > 0) {
		buf[n] = '\0';
		puts("JNU init: rewind sample: ");
		puts(buf);
		puts("\n");
	}

	(void)close(fd);
}

static char *const hello_argv[] = {"hello", 0};
static char *const fuzz_argv[] = {"fuzz", 0};
static char *const minix_argv[] = {"hello-minix", 0};

static void fork_exec_wait(const char *path, char *const argv[])
{
	int status = 0;
	int child = fork();
	if (child == 0) {
		int err = execve(path, argv, 0);

		puts("JNU init: execve failed for ");
		puts(path);
		puts(" rc=");
		put_uint((unsigned)(-err));
		puts("\n");
		exit(127);
	}

	if (child > 0) {
		(void)waitpid(child, &status);
		puts("JNU init: child ");
		put_uint((unsigned)child);
		puts(" exited ");
		put_uint((unsigned)status);
		puts("\n");
	} else {
		puts("JNU init: fork failed\n");
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	puts("JNU init: hello from ring 3\n");
	puts("JNU init: pid ");
	put_uint((unsigned)getpid());
	puts("\n");

	read_minix_file();

	fork_exec_wait("/bin/hello", hello_argv);
	fork_exec_wait("/bin/fuzz", fuzz_argv);
	fork_exec_wait("/hello", minix_argv);

	if (execve("/no/such/path", hello_argv, 0) < 0) {
		puts("JNU init: missing execve path rejected\n");
	}

	return 0;
}
