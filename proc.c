#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct proc *queue[5][NPROC];
int q_tail[5] = {-1, -1, -1, -1, -1}, q_ticks_max[5] = {1, 2, 4, 8, 16}, q_ticks[5] = {0,0,0,0,0};
int add_proc_to_q(struct proc *p, int q_no);
int remove_proc_from_q(struct proc *p, int q_no);
static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

void change_q_flag(struct proc* p){
	acquire(&ptable.lock);
	p-> change_q = 1;
	release(&ptable.lock);
}

void incr_curr_ticks(struct proc *p){
	acquire(&ptable.lock);
	p->curr_ticks++;
	p->ticks[p->queue]++;
	release(&ptable.lock);
}
// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  //***tweakinit***
  acquire(&tickslock);
  p->ctime = ticks;
  release(&tickslock);
  //***tweatend***

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
  p -> pbs_yield_flag = 0;
  p -> priority = 60;
		p->curr_ticks = 0;
		p->queue = 0;
    p -> rtime = 0;
    p -> iotime = 0;
		p->enter = 0;
		for(int i=0; i<5; i++)
			p->ticks[i] = 0;
  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  #ifdef MLFQ
  		add_proc_to_q(p, 0);
	#endif
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  #ifdef MLFQ
		// cprintf("Adding Proces %d to Queue 0\n", np->pid);
  		add_proc_to_q(np, 0);
	#endif
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }
  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;

  //***
  curproc -> etime = ticks;
  //***

  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        #ifdef MLFQ
          remove_proc_from_q(p, p -> queue);
        #endif
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//***
int waitx(int* wtime, int* rtime){
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    //scanning through table for exited children
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC] ; p++){
      if( p -> parent != curproc )
        continue;
      havekids = 1;
      if( p -> state == ZOMBIE ){
        //Have found one of those

        //time to change times
        *rtime = p -> rtime;
        *wtime = p -> etime - p -> ctime - p -> rtime;

        pid = p -> pid;
        kfree(p -> kstack);
        p -> kstack = 0;
        freevm(p -> pgdir);
        p -> pid = 0;
        p -> parent = 0;
        p -> name[0] = 0;
        p -> killed = 0;
        p -> state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    //Don't wait if no children is present
    if( !havekids || curproc -> killed ){
      release(&ptable.lock);
      return -1;
    }

    //Wait if any child was found
    sleep(curproc, &ptable.lock);     //DOC: wait-sleep
  }
}
//***

int set_priority(int pid, int priority){
  // cprintf("atleast %d\n", pid);
  struct proc *p;
  int to_yield = 0, old_priority = 0;

  for(p = ptable.proc ; p < &ptable.proc[NPROC] ; p++){
    // cprintf("pid is %d and p is %d\n", pid, p -> pid);
    if( p -> pid == pid ){
      // cprintf("there\n");
      to_yield = 0;
      acquire(&ptable.lock);
      old_priority = p -> priority;
      p -> priority = priority;
      cprintf("Changed priority of process %d from %d to %d\n", p -> pid, old_priority, p -> priority);
      if( old_priority > p -> priority)
        to_yield = 1;
      release(&ptable.lock);
      break;
    }
  }
  if( to_yield == 1 )
    yield();
  return old_priority;
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    #ifdef RR
      struct proc *p;
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
          continue;

        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;

        swtch(&(c->scheduler), p->context);
        switchkvm();

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
    #else
    #ifdef FCFS
      // cprintf("here\n");
      struct proc *p;
      struct proc *min_proc = 0;
      for(p = ptable.proc ; p < &ptable.proc[NPROC] ; p++){
        if( p -> state != RUNNABLE )
          continue;
        if( min_proc == 0 )
          min_proc = p;
        else if( p -> ctime < min_proc -> ctime )
          min_proc = p;
      }
      if( min_proc != 0 && min_proc -> state == RUNNABLE ){
        // #ifdef T
          cprintf("Process %s with a pid %d and start time %d has started execution\n", min_proc -> name, min_proc -> pid, min_proc -> ctime);
        // #endif
        p = min_proc;
        c -> proc = p;
        switchuvm(p);
        p -> num_run++;
        p -> state = RUNNING;
        swtch(&(c -> scheduler), p -> context);
        switchkvm();
        c -> proc = 0;
      }
    #else
    #ifdef PBS
      struct proc *p;
      struct proc *min_priority = 0;

      for( p = ptable.proc ; p < &ptable.proc[NPROC] ; p++){
        if( p -> state != RUNNABLE )
          continue;
        if( min_priority == 0 )
          min_priority = p;
        else if( p -> priority < min_priority -> priority )
          min_priority = p;
      }
      if( min_priority == 0 ){
        release(&ptable.lock);
        continue;
      }
      for(p = ptable.proc ; p < &ptable.proc[NPROC] ; p++){
        struct proc *q;
        int flag = 0;
        for(q = ptable.proc ; q < &ptable.proc[NPROC] ; q++){
          if( q -> state != RUNNABLE )
            continue;
          if( q -> priority < min_priority -> priority )
            flag = 1;
        }
        if( flag == 1 )
          break;
        if( p -> state != RUNNABLE )
          continue;
        else if( p -> priority == min_priority -> priority ){
          cprintf("Process %s with pid %d and priority %d started running\n", p -> name, p -> pid, p -> priority);
          c -> proc = p;
          switchuvm(p);
          p -> num_run++;
          p -> state = RUNNING;
          swtch(&(c -> scheduler), p -> context);
          switchkvm();
          c -> proc = 0;
        }
      }
    #else
    #ifdef MLFQ
			for(int i=1; i < 5; i++){
				for(int j=0; j <= q_tail[i]; j++){
					struct proc *p = queue[i][j];
					int age = ticks - p->enter;
					if(age > 30){
						remove_proc_from_q(p, i);
							cprintf("Process %d moved up to queue %d due to age time %d\n", p->pid, i-1, age);
						add_proc_to_q(p, i-1);
					}

				}
			}
			struct proc *p =0;

			for(int i=0; i < 5; i++){
				if(q_tail[i] >=0){
					p = queue[i][0];
					remove_proc_from_q(p, i);
					break;
				}
			}

			if(p!=0 && p->state==RUNNABLE)
			{
				p->curr_ticks++;
				p->num_run++;
				cprintf("Scheduling %s with PID %d from Queue %d with current tick %d\n",p->name, p->pid, p->queue, p->curr_ticks);
				p->ticks[p->queue]++;
				c->proc = p;
				switchuvm(p);
				p->state = RUNNING;
				swtch(&c->scheduler, p->context);
				switchkvm();
				c->proc = 0;

				if(p!=0 && p->state == RUNNABLE)
				{
					if(p->change_q == 1)
					{
						p->change_q = 0;
						p->curr_ticks = 0;
						if(p->queue != 4)
							p->queue++;
					}
					else p->curr_ticks = 0;
					add_proc_to_q(p, p->queue);

				}
			}
    #endif
    #endif
    #endif
    #endif
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void){
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void){
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void){
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan){
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
      #ifdef MLFQ
				p->curr_ticks = 0;
				add_proc_to_q(p, p->queue);
			#endif 
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan){
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid){
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
        #ifdef MLFQ
					add_proc_to_q(p, p->queue);
				#endif
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void){
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}


int add_proc_to_q(struct proc *p, int q_no){	

	for(int i=0; i < q_tail[q_no]; i++){
		if(p->pid == queue[q_no][i]->pid)
			return -1;
	}
	p->enter = ticks;
	p -> queue = q_no;
	q_tail[q_no]++;
	queue[q_no][q_tail[q_no]] = p;

	return 1;
}

int remove_proc_from_q(struct proc *p, int q_no){
	int proc_found = 0, rem = 0;
	for(int i=0; i <= q_tail[q_no]; i++){
		if(queue[q_no][i] -> pid == p->pid){
			rem = i;
			proc_found = 1;
			break;
		}
	}

	if(proc_found  == 0)
		return -1;

	for(int i = rem; i < q_tail[q_no]; i++)
		queue[q_no][i] = queue[q_no][i+1]; 

	q_tail[q_no] -= 1;
	return 1;

}

int pls(){
  static char *states[] = {
    [UNUSED] "unused",
    [EMBRYO] "embryo",
    [SLEEPING] "sleep",
    [RUNNABLE] "runable",
    [RUNNING] "running",
    [ZOMBIE] "zombie"
  };
  struct proc *p;
  // char *states;
  acquire(&ptable.lock);
  cprintf("\nPID Prioity Status    rtime wtime n_run cur_q q0 q1 q2 q3 q4\n");
  for(p = ptable.proc ; p < &ptable.proc[NPROC] ; p++){
    if( p -> pid == 0 )
      continue;
    int wtime;
    if( p -> state == ZOMBIE )
      wtime = p -> etime - p -> ctime - p -> rtime - p -> iotime;
    else
      wtime = ticks - p -> ctime - p -> rtime - p -> iotime;
    cprintf("\n %d     %d   %s     %d   %d   %d   %d   %d     %d   %d   %d   %d\n", 
      p -> pid, p -> priority, states[p -> state], p -> rtime,
      wtime, p -> num_run, p -> queue, p -> ticks[0], p -> ticks[1],
      p -> ticks[2], p -> ticks[3], p -> ticks[4]);
  }
  // cprintf("\n");
  release(&ptable.lock);
  return 1;
}