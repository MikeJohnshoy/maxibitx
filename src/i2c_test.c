// i2c_test.c
//
// Bench harness for i2c.c's bus search, i2c_find_bus_with(): no I2C
// hardware - the device nodes are empty files in a temporary directory, and
// which bus "answers" is decided by the test.
//
//   make test-i2c && ./test-i2c
//
// The preferred bus is taken when the device answers there, even if a
// lower-numbered bus has it too; otherwise the lowest-numbered other bus
// that answers, in number order (i2c-3 before i2c-22, not as text sorts);
// -1 when none does, or there is no directory to search. Names that aren't
// i2c-N device nodes are passed over.

#include "i2c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static void check(int ok, const char *what) {
  printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    failures++;
}

// Which buses the device answers on, and which were asked.
static int answering[64], asked[64], n_asked;
static int answers(int bus, uint8_t address) {
  if (address != 0x60)
    return 0;
  if (n_asked < 64)
    asked[n_asked++] = bus;
  return bus >= 0 && bus < 64 && answering[bus];
}

static void reset(void) {
  memset(answering, 0, sizeof(answering));
  n_asked = 0;
}

int main(void) {
  char dir[] = "/tmp/i2c_test_XXXXXX";
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    return 1;
  }
  const char *names[] = {"i2c-1", "i2c-3", "i2c-20", "i2c-21", "i2c-22", "i2c-2x", "i2c", "tty0"};
  char path[64];
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    FILE *f = fopen(path, "w");
    if (f)
      fclose(f);
  }

  printf("i2c_find_bus_with()\n");
  reset();
  answering[22] = answering[3] = 1;
  check(i2c_find_bus_with(22, 0x60, dir, answers) == 22 && n_asked == 1,
        "on the preferred bus: taken at once, nothing else asked");

  reset();
  answering[21] = 1;
  int bus = i2c_find_bus_with(22, 0x60, dir, answers);
  check(bus == 21, "not on the preferred bus: found on another");
  check(n_asked == 5 && asked[0] == 22 && asked[1] == 1 && asked[2] == 3 && asked[3] == 20 &&
            asked[4] == 21,
        "... asking the preferred bus first, then the others in number order, until found");

  reset();
  answering[3] = answering[20] = 1;
  check(i2c_find_bus_with(22, 0x60, dir, answers) == 3, "two others answer: the lower number");

  reset();
  check(i2c_find_bus_with(22, 0x60, dir, answers) == -1 && n_asked == 5,
        "nowhere: -1, after asking every i2c-N once (not i2c-2x, i2c or tty0)");

  reset();
  answering[22] = 1;
  check(i2c_find_bus_with(4, 0x60, dir, answers) == 22,
        "a preferred bus with no device node: the search finds it");

  reset();
  answering[1] = 1;
  check(i2c_find_bus_with(-1, 0x60, dir, answers) == 1, "no preferred bus: the search alone");

  reset();
  answering[22] = 1;
  check(i2c_find_bus_with(4, 0x60, "/nonexistent/dir", answers) == -1,
        "no directory to search: -1");

  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    unlink(path);
  }
  rmdir(dir);

  printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
  return failures ? 1 : 0;
}
