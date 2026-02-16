#include <stdio.h>

//snippet greet
void greet(const char *name) {
  printf("hello, %s\n", name);
}
//endsnippet

//snippet sum
int sum(int a, int b) {
  return a + b;
}
//endsnippet

int main(void) {
  greet("huap");
  printf("2 + 3 = %d\n", sum(2, 3));
  return 0;
}
