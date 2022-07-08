
/* Pi-hole: A black hole for Internet advertisements
*  (c) 2022 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Supervisor routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "supervisor.h"
#include "log.h"
#include "daemon.h"
// sleepms()
#include "timers.h"
// bool daemonmode, supervised
#include "args.h"

// PATH_MAX
#include <limits.h>
// waitpid()
#include <sys/wait.h>

static pid_t child_pid;

static void signal_handler(int sig, siginfo_t *si, void *unused)
{
	// Forward signal to child
	logg("### Supervisor received signal \"%s\" (%d), forwarding to PID %d", strsignal(sig), sig, child_pid);
	if(child_pid != 0)
		kill(child_pid, sig);
}

// Register ordinary signals handler
static void redirect_signals(void)
{
	struct sigaction old_action;

	// Loop over all possible signals, including real-time signals
	for(unsigned int signum = 0; signum < NSIG; signum++)
	{
		// Do not modify SIGCHLD handling
		if(signum == SIGCHLD)
			continue;

		// Catch this signal
		sigaction (signum, NULL, &old_action);
		if(old_action.sa_handler != SIG_IGN)
		{
			struct sigaction SIGaction;
			memset(&SIGaction, 0, sizeof(struct sigaction));
			SIGaction.sa_flags = SA_SIGINFO | SA_RESTART;
			sigemptyset(&SIGaction.sa_mask);
			SIGaction.sa_sigaction = &signal_handler;
			sigaction(signum, &SIGaction, NULL);
		}
	}
}

bool supervisor(int *exitcode)
{
	// Fork supervisor into background if requested
	if(supervised)
		go_daemon();

	// Start FTL in non-daemon sub-process
	bool restart = true;
	bool running = false;
	do
	{
		if(!running)
		{
			if((child_pid = fork()) == 0)
			{
				logg("### Supervisor: Starting sub-process");
				// Continue FTL operation
				daemonmode = false;
				return false;
			}
		}

		if (child_pid > 0)
		{
			// Redirect signals to the child's PID
			logg("### Supervisor: Redirecting signals to PID %d", child_pid);
			redirect_signals();

			/* the parent process calls waitpid() on the child */
			int status;
			const pid_t wrc = waitpid(child_pid, &status, WUNTRACED | WCONTINUED);
			if (wrc != -1)
			{
				if (WIFEXITED(status))
				{
					// The child process terminated normally, that is, by calling exit(3) or _exit(2), or by returning from main()
					*exitcode = WEXITSTATUS(status); // returns the exit status of the child
					logg("### Supervisor: Subprocess exited with code %d", *exitcode);
					restart = (*exitcode != 0);
					running = false;
				}
				else if(WIFSIGNALED(status))
				{
					// The child process was terminated by a signal
					const int sig = WTERMSIG(status); // returns the number of the signal that caused the child process to terminate
#ifdef WCOREDUMP
					const bool coredump = WCOREDUMP(status);
#else
					const bool coredump = false;
#endif
					logg("### Supervisor: Subprocess was terminated by external signal >>> %s <<< (%d)%s",
					     strsignal(sig), sig, coredump ? " - CORE DUMPED" : "");
					restart = true;
					running = false;
				}
				else if(WIFSTOPPED(status))
				{
					// returns true if the child process was stopped by delivery of a signal; this is possible only if the call
					// was done using WUNTRACED or when the child is being traced (see ptrace(2))
					const int sig = WSTOPSIG(status); // returns the number of the signal which caused the child to stop
					logg("### Supervisor: Subprocess was stopped by external signal >>> %s <<< (%d)", strsignal(sig), sig);
					restart = false;
					running = true;
				}
				else if(WIFCONTINUED(status))
				{
					// (since Linux 2.6.10) returns true if the child process was resumed by delivery of SIGCONT
					const int sig = SIGCONT;
					logg("### Supervisor: Subprocess was resumed by external signal >>> %s <<< (%d)", strsignal(sig), sig);
					restart = false;
					running = true;
				}
				else
				{
					/* the program didn't terminate normally */
					logg("### Supervisor: Abnormal termination of subprocess");
					restart = true;
					running = true;
				}
			}
			else
			{
				logg("### Supervisor: waitpid() failed: %s (%d)", strerror(errno), errno);
				restart = true;
				running = false;
			}
		}
		else
		{
			logg("### Supervisor: fork() failed: %s (%d)", strerror(errno), errno);
			restart = true;
			running = false;
		}

	// Delay restarting to avoid race-collisions with left-over shared memory files
	if(restart)
		sleepms(1000);
	} while( running || restart );

	logg("### Supervisor: Terminated (code %d)", *exitcode);
	// Exit with code
	return true;
}
