#include <jnu_syscall.h>

static void puts(const char *s) {
  size_t len = 0;

  while (s[len]) {
    len++;
  }
  (void)write(1, s, len);
}

static void put_uint(unsigned value) {
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

static void read_minix_file(void) {
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

int main(int argc, char **argv) {
  int status = 0;
  int child;

  (void)argc;
  (void)argv;

  puts("JNU init: hello from ring 3\n");
  puts("JNU init: pid ");
  put_uint((unsigned)getpid());
  puts("\n");

  read_minix_file();

  child = spawn("/bin/hello", 0);
  if (child >= 0) {
    (void)waitpid(child, &status);
  }

  child = spawn("/hello", 0);
  if (child >= 0) {
    (void)waitpid(child, &status);
  }
  return 0;
}
