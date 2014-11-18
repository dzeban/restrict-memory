#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>

#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/reg.h>
#include <sys/syscall.h>

#define THRESHOLD 2*1048576 // 2 MiB threshold
bool threshold = false; // Global flag indicating if threshold exceeded. 

static int debug = 0;
#define dbg(...) do {\
if (debug) \
	fprintf(stderr, __VA_ARGS__);\
}while(0);

// Invoke ptrace to trace syscall and fill regs state
int syscall_trace(pid_t pid, struct user_regs_struct *state)
{
	int status;

	ptrace(PTRACE_SYSCALL, pid, 0, 0);
	waitpid(pid, &status, 0);
	if (WIFSTOPPED(status) && WSTOPSIG(status) & 0x80)
	{
		ptrace(PTRACE_GETREGS, pid, 0, state);
		return 0;
	}
	else if (WIFEXITED(status)) 
	{
		exit(0);
	}
	else
	{
		return EINVAL;
	}
}

// Handle brk syscall.
//
// It checks the difference between initial brk value and current return value
// and if that difference exceeded threshold it sets global threshold flag (for
// subsequent mmap calls) and returns -ENOMEM for syscall.
void handle_brk(pid_t pid, struct user_regs_struct state)
{
	int status;
	static long int brk_start = 0;
	long int diff = 0;

	dbg("brk addr 0x%08X\n", state.ebx);

	// Get return value
	if (!syscall_trace(pid, &state))
	{
		dbg("brk return: 0x%08X, brk_start 0x%08X\n", state.eax, brk_start);

		if (brk_start)
		{
			diff = state.eax - brk_start;

			// If child process exceeded threshold 
			// replace brk return value with -ENOMEM
			if (diff > THRESHOLD || threshold) 
			{
				dbg("THRESHOLD!\n");
				threshold = true;
				state.eax = -ENOMEM;
				ptrace(PTRACE_SETREGS, pid, 0, &state);
			}
			else
			{
				dbg("diff 0x%08X\n", diff);
			}
		}
		else
		{
			dbg("Assigning 0x%08X to brk_start\n", state.eax);
			brk_start = state.eax;
		}
	}
}

// Handle mmap/mmap2 syscalls.
//
// It checks global threshold flag set by handle_brk. If it's set it will
// replace mmap return value with ENOMEM.
//
// mmap/mmap2 is alternative syscall for malloc, invoked when brk returning
// ENOMEM.
void handle_mmap(pid_t pid, struct user_regs_struct state)
{
	dbg("mmap call, ebx 0x%08X, ecx 0x%08X\n", state.ebx, state.ecx);

	// Get return value
	if (!syscall_trace(pid, &state))
	{
		dbg("mmap return (threshold %d), eax 0x%08X, ebx 0x%08X\n", threshold, state.eax, state.ebx);

		// mmap will be called when brk return ENOMEM,
		// thus we must fail mmap too with -ENOMEM
		if (threshold)
		{
			state.eax = -ENOMEM;
			ptrace(PTRACE_SETREGS, pid, 0, &state);
		}
	}
}

// Parent process
int tracer(pid_t pid)
{
	int status;

	// Wait for child process and set sane ptrace options
	waitpid(pid, &status, 0);
	ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);

	// Trace until child exited. 
	// There is another exit in syscall_trace() (FIXME)
	while (!WIFEXITED(status))
	{
		struct user_regs_struct state;

		if (!syscall_trace(pid, &state))
		{
			switch (state.orig_eax)
			{
				case SYS_brk:
					handle_brk(pid, state);
					break;

				case SYS_mmap:
				case SYS_mmap2:
					handle_mmap(pid, state);
					break;

				default:
					break;
			}
		}
	}

	return 0;
}

// Child process function
int tracee(char *path, char *argv[])
{
	ptrace(PTRACE_TRACEME, 0, 0, 0);
	return execv(path, argv);
}

void usage()
{
	printf("Usage: \n");
	printf("%s [-d] <program to restrict>\n\n");
	printf("    -d : print debug output\n");
}

int main(int argc, char *argv[])
{
	int opt;
	while ((opt = getopt(argc, argv, "d")) != -1)
	{
		switch (opt)
		{
			case 'd':
				debug = 1;
				break;
			default:
				perror("getopt");
				usage();
				exit(1);
		}
	}
	argv += optind;

	pid_t pid = fork();
	if (pid)
		return tracer(pid);
	else
		return tracee(argv[0], argv + 1);
}


// vim: ts=4: sw=4: noexpandtab
