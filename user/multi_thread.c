#include "user/user.h"

void test_func() {
    printf("test_func");
}

int
main(int argc, char *argv[])
{
    int tid;
    thread_create(&tid, test_func, 0);
    thread_join(tid);
    exit(0);
}