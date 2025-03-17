#include "user/user.h"

void test_func() {
    printf("test_func");
}

int
main(int argc, char *argv[])
{
    // main belong to tbc[0], aka main thread
    int tid;
    thread_create(&tid, test_func, 0);
    thread_join(tid);
    exit(0);
}