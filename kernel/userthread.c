#include "types.h"
extern void thread_exit();

void userthread(uint64 thread_func, uint64 args)
{
    ((void (*)(void*))thread_func)((void*)args);
    thread_exit(0);
}