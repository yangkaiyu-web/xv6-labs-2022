#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_ptr kpgtbl)
{
  struct proc *p;
  int th_i;

  for(p = proc; p < &proc[NPROC]; p++) {
    for (th_i = 0; th_i < 4; th_i++) {
      char *pa = kalloc();
      if(pa == 0)
        panic("kalloc");
      uint64 va = KSTACK((int) (p - proc), th_i);
      kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  int th_i;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    initlock(&(p->pagetable.lock), "user_proc_pagetable_lock");
    p->state = UNUSED;
    for(th_i = 0; th_i < 4; th_i++) {
        initlock(&(p->tcb[th_i].tlock), "thread");
        p->tcb[th_i].tid = th_i;
        p->tcb[th_i].state = UNUSED;
        p->tcb[th_i].kstack = KSTACK((int) (p - proc), th_i);
        p->tcb[th_i].trapframe = 0;
    }
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc_thread, proc field maybe none.
struct proc_thread
mythread(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc_thread p_t = c->proc_thread;
  pop_off();
  return p_t;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a main thread structure and page.
  struct trapframe * k_mem;
  if ((k_mem = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  } else {
    int i;
    for (i=0;i<4;i++) {
      p->tcb[i].trapframe = k_mem + i*TRAPFRAME_SIZE;
    }
  }

  // An empty user page table.
  p->pagetable.pagetable = proc_pagetable(p);
  if(p->pagetable.pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&(p->tcb[0].context), 0, sizeof(p->tcb[0].context));
  p->tcb[0].context.ra = (uint64)forkret;
  p->tcb[0].context.sp = p->tcb[0].kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
// all thread must be released
static void
freeproc(struct proc *p)
{
  int i;
  if (p->tcb[0].trapframe) {
    kfree((void*)(p->tcb[0].trapframe));
    p->tcb[0].trapframe = 0;
  }
  for(i = 0; i < 4; i++) {
    // acquire(&(p->tcb[i].tlock));
    p->tcb[i].state = UNUSED;
    if(p->tcb[i].trapframe) {
      p->tcb[i].trapframe = 0;
    }
    p->tcb[i].chan = 0;
    p->tcb[i].killed = 0
    // release(&(p->tcb[i].tlock));
  }
  if(p->pagetable.pagetable)
    proc_freepagetable(p->pagetable.pagetable, p->sz);
  p->pagetable.pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_ptr
proc_pagetable(struct proc *p)
{
  pagetable_ptr pagetable;

  pagetable = (pagetable_ptr) kalloc();
  if(pagetable == 0)
    return pagetable;
  memset(pagetable, 0, PGSIZE);

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the 4 trapframes' page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME_START, PGSIZE,
              (uint64)(p->tcb[0].trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_ptr pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME_START, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(&(p->pagetable), initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->tcb[0].trapframe->epc = 0;      // user program counter
  p->tcb[0].trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = mythread().p;

  acquire(&(p->pagetable.lock));
  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable.pagetable, sz, sz + n, PTE_W)) == 0) {
      release(&(p->pagetable.lock));
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable.pagetable, sz, sz + n);
  }
  p->sz = sz;
  release(&(p->pagetable.lock));
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc_thread proc_th = mythread();
  struct proc *p = proc_th.p;
  int tid = proc_th.tid;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy mask for syscall trace
  np->mask = p->mask;
  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->tcb[0].trapframe) = *(p->tcb[tid].trapframe);

  // Cause fork to return 0 in the child.
  np->tcb[0].trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  np->tcb[0].state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc_thread p_t = mythread();
  struct proc* p = p_t.p;
  wait_all_thread_exit(p);

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc_thread p_t = mythread();
  struct proc *p = p_t.p;

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable.pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc_thread.p = 0;
  c->proc_thread.tid = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      int th_i;
      for(th_i = 0; th_i < 4; th_i++) {
        acquire(&(p->tcb[th_i].tlock));
        if(p->tcb[th_i].state == RUNNABLE) {
          // Switch to chosen process.  It is the process's job
          // to release its lock and then reacquire it
          // before jumping back to us.
          p->tcb[th_i].state = RUNNING;
          c->proc_thread = (struct proc_thread){p, th_i};
          swtch(&c->context, &p->tcb[th_i].context);

          // Process is done running for now.
          // It should have changed its p->state before coming back.
          c->proc_thread.p = 0;
          c->proc_thread.tid = 0;
        }
        release(&(p->tcb[th_i].tlock));
      }
    }
  }
}

// Switch to scheduler.  Must hold only thread.lock
// and have changed thread.state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc_thread p_t = mythread();
  struct proc *p = p_t.p;
  int tid = p_t.tid;

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->tcb[tid].context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;
  struct proc_thread p_t = mythread();
  // Still holding p->lock from scheduler.
  release(&(p_t.p->tcb[p_t.tid].tlock));

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc_thread p_t = mythread();
  struct proc *p = p_t.p;

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&(p->tcb[p_t.tid].tlock));  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->tcb[p_t.tid].chan = chan;
  p->tcb[p_t.tid].state = SLEEPING;

  sched();

  // Tidy up.
  p->tcb[p_t.tid].chan = 0;

  // Reacquire original lock.
  release(&(p->tcb[p_t.tid].tlock));
  acquire(lk);
}

// Wake up all thread sleeping on chan.
// Must be called without any thread.lock.
void
wakeup(void *chan)
{
  struct proc *p;
  struct proc_thread my_th;
  int i;
  for(p = proc; p < &proc[NPROC]; p++) {
    for(i = 0; i < 4; i++) {
      my_th = mythread();
      if((p == my_th.p) && (i == my_th.tid)) {
        continue;
      } else {
        acquire(&(p->tcb[i].tlock));
        if(p->tcb[i].state == SLEEPING && p->tcb[i].chan == chan) {
          p->tcb[i].state = RUNNABLE;
        }
        release(&(p->tcb[i].tlock));
      }
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      int i;
      for (i=0;i<4;i++) {
        acquire(&(p->tcb[i].tlock));
        p->tcb[i].killed = 1;
        if(p->tcb[i].state == SLEEPING){
          // Wake process from sleep().
          p->tcb[i].state = RUNNABLE;
        }
        release(&(p->tcb[i].tlock));
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  int i;
  for (i=0; i<4; i++) {
    acquire(&(p->tcb[i].tlock));
    p->tcb[i].killed = 1;
    if(p->tcb[i].state == SLEEPING){
      // Wake tread from sleep().
      p->tcb[i].state = RUNNABLE;
    }
    release(&(p->tcb[i].tlock));
  }
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

int thread_killed(struct proc_thread p_t)
{
  int k;
  struct thread_cb* th = &(p_t.p->tcb[p_t.tid]);

  acquire(&(th->tlock));
  k = th->killed;
  release(&(th->tlock));

}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable.pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable.pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// get the number of processes whose state is not UNUSED
int
get_nproc()
{
  int n = 0;
  for(int i=0; i<NPROC; i++){
    if(proc[i].state != UNUSED){
      n++;
    }
  }
  return n;
}

void
wait_all_thread_exit(struct proc* p) {
  int i;

  int all_exit = 0;
  acquire(&p->wait_thread_lock);
  for (i = 1; i < 4; i++) {
    struct thread_cb* t = &p->tcb[i];
    acquire(&(t->tlock));
    for(;;) {
      if (t->state != ZOMBIE) {
        release(&(t->tlock));
        sleep(t, &p->wait_thread_lock);
        acquire(&(t->tlock));
      } else {
        break;
      }
    }
    release(&(t->tlock));
  }
  release(&(p->wait_thread_lock));
}
