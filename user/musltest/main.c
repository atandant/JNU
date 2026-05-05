#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int check(int ok, const char *what)
{
	if (ok) {
		return 0;
	}
	printf("musltest: %s failed\n", what);
	return 1;
}

int main(void)
{
	const char msg[] = "minix rw\n";
	char buf[32];
	int fd;
	int failed = 0;
	ssize_t n;

	(void)unlink("/mt-file");
	(void)unlink("/mt-dir/renamed");
	(void)rmdir("/mt-dir");

	fd = creat("/mt-file", 0666);
	if (fd < 0) {
		printf("musltest: creat failed\n");
		return 1;
	}
	failed |= check(write(fd, msg, sizeof(msg) - 1) == (ssize_t)sizeof(msg) - 1,
			"write");
	failed |= check(fsync(fd) == 0, "fsync");
	failed |= check(ftruncate(fd, 5) == 0, "ftruncate");
	failed |= check(close(fd) == 0, "close created file");
	if (failed) {
		return 1;
	}

	fd = open("/mt-file", 0);
	if (fd < 0) {
		printf("musltest: open failed\n");
		return 1;
	}
	memset(buf, 0, sizeof(buf));
	n = read(fd, buf, sizeof(buf) - 1);
	failed |= check(n == 5, "read truncated size");
	failed |= check(memcmp(buf, "minix", 5) == 0, "read truncated data");
	failed |= check(close(fd) == 0, "close reopened file");

	failed |= check(mkdir("/mt-dir", 0777) == 0, "mkdir");
	failed |= check(rename("/mt-file", "/mt-dir/renamed") == 0, "rename");
	failed |= check(unlink("/mt-dir/renamed") == 0, "unlink");
	failed |= check(rmdir("/mt-dir") == 0, "rmdir");

	if (failed) {
		return 1;
	}
	printf("musltest: minix rw syscalls OK\n");
	return 0;
}
