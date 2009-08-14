/*
	This file is part of Warzone 2100.
	Copyright (C) 2007-2009  Warzone Resurrection Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "lib/framework/frame.h"
#include "lib/framework/string_ext.h"
#include "exceptionhandler.h"
#include "dumpinfo.h"

#if defined(WZ_OS_WIN)

# include "dbghelp.h"
# include "exchndl.h"

#if !defined(WZ_CC_MINGW)
static LPTOP_LEVEL_EXCEPTION_FILTER prevExceptionHandler = NULL;

/**
 * Exception handling on Windows.
 * Ask the user whether he wants to safe a Minidump and then dump it into the temp directory.
 *
 * \param pExceptionInfo Information on the exception, passed from Windows
 * \return whether further exception handlers (i.e. the Windows internal one) should be invoked
 */
static LONG WINAPI windowsExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
	LPCSTR applicationName = "Warzone 2100";

	char miniDumpPath[PATH_MAX] = {'\0'}, resultMessage[PATH_MAX] = {'\0'};

	// Write to temp dir, to support unprivileged users
	if (!GetTempPathA(sizeof(miniDumpPath), miniDumpPath))
	{
		sstrcpy(miniDumpPath, "c:\\temp\\");
	}

	// Append the filename
	sstrcat(miniDumpPath, "warzone2100.mdmp");

	/*
	Alternative:
	GetModuleFileName( NULL, miniDumpPath, MAX_PATH );

	// Append extension
	sstrcat(miniDumpPath, ".mdmp");
	*/

	if ( MessageBoxA( NULL, "Warzone crashed unexpectedly, would you like to save a diagnostic file?", applicationName, MB_YESNO ) == IDYES )
	{
		HANDLE miniDumpFile = CreateFileA( miniDumpPath, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );

		if (miniDumpFile != INVALID_HANDLE_VALUE)
		{
			MINIDUMP_USER_STREAM uStream = { LastReservedStream+1, strlen(PACKAGE_VERSION), PACKAGE_VERSION };
			MINIDUMP_USER_STREAM_INFORMATION uInfo = { 1, &uStream };
			MINIDUMP_EXCEPTION_INFORMATION eInfo = { GetCurrentThreadId(), pExceptionInfo, false };

			if ( MiniDumpWriteDump(
				 	GetCurrentProcess(),
					GetCurrentProcessId(),
					miniDumpFile,
					MiniDumpNormal,
					pExceptionInfo ? &eInfo : NULL,
					&uInfo,
					NULL ) )
			{
				snprintf(resultMessage, sizeof(resultMessage), "Saved dump file to '%s'", miniDumpPath);
			}
			else
			{
				snprintf(resultMessage, sizeof(resultMessage), "Failed to save dump file to '%s' (error %d)", miniDumpPath, (int)GetLastError());
			}

			CloseHandle(miniDumpFile);
		}
		else
		{
			snprintf(resultMessage, sizeof(resultMessage), "Failed to create dump file '%s' (error %d)", miniDumpPath, (int)GetLastError());
		}

		MessageBoxA( NULL, resultMessage, applicationName, MB_OK );
	}

	if (prevExceptionHandler)
		return prevExceptionHandler(pExceptionInfo);
	else
		return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#elif defined(WZ_OS_UNIX) && !defined(WZ_OS_MAC)

// C99 headers:
# include <stdint.h>
# include <signal.h>
# include <string.h>

// POSIX headers:
# include <unistd.h>
# include <fcntl.h>
# include <time.h>
# include <sys/types.h>
# include <sys/stat.h>
# include <sys/wait.h>
# include <sys/utsname.h>

// GNU extension for backtrace():
# if defined(__GLIBC__)
#  include <execinfo.h>
#  define MAX_BACKTRACE 20
# endif


# define MAX_PID_STRING 16
# define MAX_DATE_STRING 256


typedef void(*SigActionHandler)(int, siginfo_t *, void *);


#ifdef WZ_OS_MAC
static struct sigaction oldAction[32];
#elif defined(_NSIG)
static struct sigaction oldAction[_NSIG];
#else
static struct sigaction oldAction[NSIG];
#endif


static struct utsname sysInfo;
static BOOL gdbIsAvailable = false, programIsAvailable = false, sysInfoValid = false;
static char
	executionDate[MAX_DATE_STRING] = {'\0'},
	programPID[MAX_PID_STRING] = {'\0'},
	programPath[PATH_MAX] = {'\0'},
	gdbPath[PATH_MAX] = {'\0'};


/**
 * Signal number to string mapper.
 * Also takes into account the signal code with details about the signal.
 *
 * \param signum Signal number
 * \param sigcode Signal code
 * \return String with the description of the signal. "Unknown signal" when no description is available.
 */
static const char * wz_strsignal(int signum, int sigcode)
{
	switch (signum)
	{
		case SIGABRT:
			return "SIGABRT: Process abort signal";
		case SIGALRM:
			return "SIGALRM: Alarm clock";
		case SIGBUS:
			switch (sigcode)
			{
				case BUS_ADRALN:
					return "SIGBUS: Access to an undefined portion of a memory object: Invalid address alignment";
				case BUS_ADRERR:
					return "SIGBUS: Access to an undefined portion of a memory object: Nonexistent physical address";
				case BUS_OBJERR:
					return "SIGBUS: Access to an undefined portion of a memory object: Object-specific hardware error";
				default:
					return "SIGBUS: Access to an undefined portion of a memory object";
			}
		case SIGCHLD:
			switch (sigcode)
			{
				case CLD_EXITED:
					return "SIGCHLD: Child process terminated, stopped, or continued: Child has exited";
				case CLD_KILLED:
					return "SIGCHLD: Child process terminated, stopped, or continued: Child has terminated abnormally and did not create a core file";
				case CLD_DUMPED:
					return "SIGCHLD: Child process terminated, stopped, or continued: Child has terminated abnormally and created a core file";
				case CLD_TRAPPED:
					return "SIGCHLD: Child process terminated, stopped, or continued: Traced child has trapped";
				case CLD_STOPPED:
					return "SIGCHLD: Child process terminated, stopped, or continued: Child has stopped";
				case CLD_CONTINUED:
					return "SIGCHLD: Child process terminated, stopped, or continued: Stopped child has continued";
			}
		case SIGCONT:
			return "SIGCONT: Continue executing, if stopped";
		case SIGFPE:
			switch (sigcode)
			{
				case FPE_INTDIV:
					return "SIGFPE: Erroneous arithmetic operation: Integer divide by zero";
				case FPE_INTOVF:
					return "SIGFPE: Erroneous arithmetic operation: Integer overflow";
				case FPE_FLTDIV:
					return "SIGFPE: Erroneous arithmetic operation: Floating-point divide by zero";
				case FPE_FLTOVF:
					return "SIGFPE: Erroneous arithmetic operation: Floating-point overflow";
				case FPE_FLTUND:
					return "SIGFPE: Erroneous arithmetic operation: Floating-point underflow";
				case FPE_FLTRES:
					return "SIGFPE: Erroneous arithmetic operation: Floating-point inexact result";
				case FPE_FLTINV:
					return "SIGFPE: Erroneous arithmetic operation: Invalid floating-point operation";
				case FPE_FLTSUB:
					return "SIGFPE: Erroneous arithmetic operation: Subscript out of range";
				default:
					return "SIGFPE: Erroneous arithmetic operation";
			};
		case SIGHUP:
			return "SIGHUP: Hangup";
		case SIGILL:
			switch (sigcode)
			{
				case ILL_ILLOPC:
					return "SIGILL: Illegal instruction: Illegal opcode";
				case ILL_ILLOPN:
					return "SIGILL: Illegal instruction: Illegal operand";
				case ILL_ILLADR:
					return "SIGILL: Illegal instruction: Illegal addressing mode";
				case ILL_ILLTRP:
					return "SIGILL: Illegal instruction: Illegal trap";
				case ILL_PRVOPC:
					return "SIGILL: Illegal instruction: Privileged opcode";
				case ILL_PRVREG:
					return "SIGILL: Illegal instruction: Privileged register";
				case ILL_COPROC:
					return "SIGILL: Illegal instruction: Coprocessor error";
				case ILL_BADSTK:
					return "SIGILL: Illegal instruction: Internal stack error";
				default:
					return "SIGILL: Illegal instruction";
			}
		case SIGINT:
			return "SIGINT: Terminal interrupt signal";
		case SIGKILL:
			return "SIGKILL: Kill";
		case SIGPIPE:
			return "SIGPIPE: Write on a pipe with no one to read it";
		case SIGQUIT:
			return "SIGQUIT: Terminal quit signal";
		case SIGSEGV:
			switch (sigcode)
			{
				case SEGV_MAPERR:
					return "SIGSEGV: Invalid memory reference: Address not mapped to object";
				case SEGV_ACCERR:
					return "SIGSEGV: Invalid memory reference: Invalid permissions for mapped object";
				default:
					return "SIGSEGV: Invalid memory reference";
			}
		case SIGSTOP:
			return "SIGSTOP: Stop executing";
		case SIGTERM:
			return "SIGTERM: Termination signal";
		case SIGTSTP:
			return "SIGTSTP: Terminal stop signal";
		case SIGTTIN:
			return "SIGTTIN: Background process attempting read";
		case SIGTTOU:
			return "SIGTTOU: Background process attempting write";
		case SIGUSR1:
			return "SIGUSR1: User-defined signal 1";
		case SIGUSR2:
			return "SIGUSR2: User-defined signal 2";
#if _XOPEN_UNIX
		case SIGPOLL:
			switch (sigcode)
			{
				case POLL_IN:
					return "SIGPOLL: Pollable event: Data input available";
				case POLL_OUT:
					return "SIGPOLL: Pollable event: Output buffers available";
				case POLL_MSG:
					return "SIGPOLL: Pollable event: Input message available";
#if defined(POLL_ERR) && defined(POLL_HUP) && (POLL_ERR != POLL_HUP)
				case POLL_ERR:
					return "SIGPOLL: Pollable event: I/O error";
#endif
				case POLL_PRI:
					return "SIGPOLL: Pollable event: High priority input available";
#if defined(POLL_ERR) && defined(POLL_HUP) && (POLL_ERR != POLL_HUP)
				case POLL_HUP:
					return "SIGPOLL: Pollable event: Device disconnected.";
#endif
	/* Work around the fact that the FreeBSD kernel uses the same value for
	 * POLL_ERR and POLL_HUP. See
	 * http://www.freebsd.org/cgi/cvsweb.cgi/src/sys/sys/signal.h (version
	 * 1.47 introduced these constants with the same values).
	 */
#if defined(POLL_ERR) && defined(POLL_HUP) && (POLL_ERR == POLL_HUP)
				case POLL_ERR:
					return "SIGPOLL: Pollable event: \"I/O error\" or \"Device disconnected\".";
#endif
				default:
					return "SIGPOLL: Pollable event";
			}
		case SIGPROF:
			return "SIGPROF: Profiling timer expired";
		case SIGSYS:
			return "SIGSYS: Bad system call";
		case SIGTRAP:
			switch (sigcode)
			{
				case TRAP_BRKPT:
					return "SIGTRAP: Trace/breakpoint trap: Process breakpoint";
				case TRAP_TRACE:
					return "SIGTRAP: Trace/breakpoint trap: Process trace trap";
				default:
					return "SIGTRAP: Trace/breakpoint trap";
			}
#endif // _XOPEN_UNIX
		case SIGURG:
			return "SIGURG: High bandwidth data is available at a socket";
#if _XOPEN_UNIX
		case SIGVTALRM:
			return "SIGVTALRM: Virtual timer expired";
		case SIGXCPU:
			return "SIGXCPU: CPU time limit exceeded";
		case SIGXFSZ:
			return "SIGXFSZ: File size limit exceeded";
#endif // _XOPEN_UNIX
		default:
			return "Unknown signal";
	}
}


/**
 * Set signal handlers for fatal signals on POSIX systems
 *
 * \param signalHandler Pointer to the signal handler function
 */
static void setFatalSignalHandler(SigActionHandler signalHandler)
{
	struct sigaction new_handler;

	new_handler.sa_sigaction = signalHandler;
	sigemptyset(&new_handler.sa_mask);
	new_handler.sa_flags = SA_SIGINFO;

	sigaction(SIGABRT, NULL, &oldAction[SIGABRT]);
	if (oldAction[SIGABRT].sa_handler != SIG_IGN)
		sigaction(SIGABRT, &new_handler, NULL);

	sigaction(SIGBUS, NULL, &oldAction[SIGBUS]);
	if (oldAction[SIGBUS].sa_handler != SIG_IGN)
		sigaction(SIGBUS, &new_handler, NULL);

	sigaction(SIGFPE, NULL, &oldAction[SIGFPE]);
	if (oldAction[SIGFPE].sa_handler != SIG_IGN)
		sigaction(SIGFPE, &new_handler, NULL);

	sigaction(SIGILL, NULL, &oldAction[SIGILL]);
	if (oldAction[SIGILL].sa_handler != SIG_IGN)
		sigaction(SIGILL, &new_handler, NULL);

	sigaction(SIGQUIT, NULL, &oldAction[SIGQUIT]);
	if (oldAction[SIGQUIT].sa_handler != SIG_IGN)
		sigaction(SIGQUIT, &new_handler, NULL);

	sigaction(SIGSEGV, NULL, &oldAction[SIGSEGV]);
	if (oldAction[SIGSEGV].sa_handler != SIG_IGN)
		sigaction(SIGSEGV, &new_handler, NULL);

#if _XOPEN_UNIX
	sigaction(SIGSYS, NULL, &oldAction[SIGSYS]);
	if (oldAction[SIGSYS].sa_handler != SIG_IGN)
		sigaction(SIGSYS, &new_handler, NULL);

	sigaction(SIGTRAP, NULL, &oldAction[SIGTRAP]);
	if (oldAction[SIGTRAP].sa_handler != SIG_IGN)
		sigaction(SIGTRAP, &new_handler, NULL);

	sigaction(SIGXCPU, NULL, &oldAction[SIGXCPU]);
	if (oldAction[SIGXCPU].sa_handler != SIG_IGN)
		sigaction(SIGXCPU, &new_handler, NULL);

	sigaction(SIGXFSZ, NULL, &oldAction[SIGXFSZ]);
	if (oldAction[SIGXFSZ].sa_handler != SIG_IGN)
		sigaction(SIGXFSZ, &new_handler, NULL);
#endif // _XOPEN_UNIX
}

/**
 * Spawn a new GDB process and attach it to the current process.
 *
 * @param dumpFile          a POSIX file descriptor to write GDB's output to,
 *                          it will also be used to write failure messages to.
 * @param[out] gdbWritePipe a POSIX file descriptor linked to GDB's stdin.
 *
 * @return 0 if we failed to spawn a new process, a non-zero process ID if we
 *           successfully spawned a new process.
 *
 * @post If the function returned a non-zero process ID a new process has
 *       successfully been spawned. This doesn't mean that 'gdb' was
 *       successfully started though. If 'gdb' failed to start the read end of
 *       the pipe will be closed, also the spawned process will give 1 as its
 *       return code.
 *
 * @post If the function returned a non-zero process ID *gdbWritePipe will
 *       contain a valid POSIX file descriptor representing GDB's stdin. If the
 *       function was unsuccessful and returned zero *gdbWritePipe's value will
 *       be unchanged.
 */
static pid_t execGdb(int const dumpFile, int* gdbWritePipe)
{
	/* Check if the "bare minimum" is available: GDB and an absolute path
	 * to our program's binary.
	 */
	if (!programIsAvailable
	 || !gdbIsAvailable)
	{
		write(dumpFile, "No extended backtrace dumped:\n",
		         strlen("No extended backtrace dumped:\n"));

		if (!programIsAvailable)
		{
			write(dumpFile, "- Program path not available\n",
			         strlen("- Program path not available\n"));
		}
		if (!gdbIsAvailable)
		{
			write(dumpFile, "- GDB not available\n",
			         strlen("- GDB not available\n"));
		}

		return 0;
	}

	// Create a pipe to use for communication with 'gdb'
	int gdbPipe[2];
	if (pipe(gdbPipe) == -1)
	{
		write(dumpFile, "Pipe failed\n",
		         strlen("Pipe failed\n"));

		printf("Pipe failed\n");

		return 0;
	}

	// Fork a new child process
	const pid_t pid = fork();
	if (pid == -1)
	{
		write(dumpFile, "Fork failed\n",
		         strlen("Fork failed\n"));

		printf("Fork failed\n");

		// Clean up our pipe
		close(gdbPipe[0]);
		close(gdbPipe[1]);

		return 0;
	}

	// Check to see if we're the parent
	if (pid != 0)
	{
		// Return the write end of the pipe
		*gdbWritePipe = gdbPipe[1];

		return pid;
	}

	char *gdbArgv[] = { gdbPath, programPath, programPID, NULL };
	char *gdbEnv[] = { NULL };

	close(gdbPipe[1]); // No output to pipe

	dup2(gdbPipe[0], STDIN_FILENO); // STDIN from pipe
	dup2(dumpFile, STDOUT_FILENO); // STDOUT to dumpFile

	write(dumpFile, "GDB extended backtrace:\n",
			strlen("GDB extended backtrace:\n"));

	/* If execve() is successful it effectively prevents further
	 * execution of this function.
	 */
	execve(gdbPath, (char **)gdbArgv, (char **)gdbEnv);

	// If we get here it means that execve failed!
	write(dumpFile, "execcv(\"gdb\") failed\n",
	         strlen("execcv(\"gdb\") failed\n"));

	// Terminate the child, indicating failure
	exit(1);
}

/**
 * Dumps a backtrace of the stack to the given output stream.
 *
 * @param dumpFile a POSIX file descriptor to write the resulting backtrace to.
 *
 * @return false if any failure occurred, preventing a full "extended"
 *               backtrace.
 */
static bool gdbExtendedBacktrace(int const dumpFile)
{
	// Spawn a GDB instance and retrieve a pipe to its stdin
	int gdbPipe;
	const pid_t pid = execGdb(dumpFile, &gdbPipe);
	if (pid == 0)
	{
		return false;
	}

	                                  // Retrieve a full stack backtrace
	static const char gdbCommands[] = "backtrace full\n"

	                                  // Move to the stack frame where we triggered the crash
	                                  "frame 4\n"

	                                  // Show the assembly code associated with that stack frame
	                                  "disassemble\n"

	                                  // Show the content of all registers
	                                  "info registers\n"
	                                  "quit\n";

	write(gdbPipe, gdbCommands, sizeof(gdbCommands));

	/* Flush our end of the pipe to make sure that GDB has all commands
	 * directly available to it.
	 */
	fsync(gdbPipe);

	// Wait for our child to terminate
	int status;
	const pid_t wpid = waitpid(pid, &status, 0);

	// Clean up our end of the pipe
	close(gdbPipe);

	// waitpid(): on error, -1 is returned
	if (wpid == -1)
	{
		write(dumpFile, "GDB failed\n",
		         strlen("GDB failed\n"));
		printf("GDB failed\n");

		return false;
	}

	/* waitpid(): on success, returns the process ID of the child whose
	 * state has changed
	 *
	 * We only have one child, from our fork() call above, thus these PIDs
	 * should match.
	 */
	assert(pid == wpid);

	/* Check wether our child (which presumably was GDB, but doesn't
	 * necessarily have to be) didn't terminate normally or had a non-zero
	 * return code.
	 */
	if (!WIFEXITED(status)
	 || WEXITSTATUS(status) != 0)
	{
		write(dumpFile, "GDB failed\n",
		         strlen("GDB failed\n"));
		printf("GDB failed\n");

		return false;
	}

	return true;
}


/**
 * Exception (signal) handling on POSIX systems.
 * Dumps info about the system incl. backtrace (when GLibC or GDB is present) to /tmp/warzone2100.gdmp
 *
 * \param signum Signal number
 * \param siginfo Signal info
 * \param sigcontext Signal context
 */
static void posixExceptionHandler(int signum, siginfo_t * siginfo, WZ_DECL_UNUSED void * sigcontext)
{
	static sig_atomic_t allreadyRunning = 0;

	if (allreadyRunning)
		raise(signum);
	allreadyRunning = 1;

# if defined(__GLIBC__)
	void * btBuffer[MAX_BACKTRACE] = {NULL};
	uint32_t btSize = backtrace(btBuffer, MAX_BACKTRACE);
# endif

	// XXXXXX will be converted into random characters by mkstemp(3)
	static const char gdmpPath[] = "/tmp/warzone2100.gdmp-XXXXXX";

	char dumpFilename[sizeof(gdmpPath)];
	sstrcpy(dumpFilename, gdmpPath);

	const int dumpFile = mkstemp(dumpFilename);


	if (dumpFile == -1)
	{
		printf("Failed to create dump file '%s'", dumpFilename);
		return;
	}


	// Dump a generic info header
	dbgDumpHeader(dumpFile);


	write(dumpFile, "Dump caused by signal: ", strlen("Dump caused by signal: "));

	const char * signal = wz_strsignal(siginfo->si_signo, siginfo->si_code);
	write(dumpFile, signal, strlen(signal));
	write(dumpFile, "\n\n", 2);

	dbgDumpLog(dumpFile); // dump out the last several log calls

# if defined(__GLIBC__)
	// Dump raw backtrace in case GDB is not available or fails
	write(dumpFile, "GLIBC raw backtrace:\n", strlen("GLIBC raw backtrace:\n"));
	backtrace_symbols_fd(btBuffer, btSize, dumpFile);
	write(dumpFile, "\n", 1);
# else
	write(dumpFile, "GLIBC not available, no raw backtrace dumped\n\n",
		  strlen("GLIBC not available, no raw backtrace dumped\n\n"));
# endif


	// Make sure everything is written before letting GDB write to it
	fsync(dumpFile);

	// Use 'gdb' to provide an "extended" backtrace
	gdbExtendedBacktrace(dumpFile);

	printf("Saved dump file to '%s'\n"
	       "If you create a bugreport regardings this crash, please include this file.\n", dumpFilename);
	close(dumpFile);


	sigaction(signum, &oldAction[signum], NULL);
	raise(signum);
}


#endif // WZ_OS_*

#if defined(WZ_OS_UNIX) && !defined(WZ_OS_MAC)
static bool fetchProgramPath(char * const programPath, size_t const bufSize, const char * const programCommand)
{
	// Construct the "which $(programCommand)" string
	char whichProgramCommand[PATH_MAX];
	snprintf(whichProgramCommand, sizeof(whichProgramCommand), "which %s", programCommand);

	/* Fill the output buffer with zeroes so that we can rely on the output
	 * string being NUL-terminated.
	 */
	memset(programPath, 0, bufSize);

	/* Execute the "which" command (constructed above) and collect its
	 * output in programPath.
	 */
	FILE * const whichProgramStream = popen(whichProgramCommand, "r");
	size_t const bytesRead = fread(programPath, 1, bufSize, whichProgramStream);
	pclose(whichProgramStream);

	// Check whether our buffer is too small, indicate failure if it is
	if (bytesRead == bufSize)
	{
		debug(LOG_WARNING, "Could not retrieve full path to \"%s\", as our buffer is too small. This may prevent creation of an extended backtrace.", programCommand);
		return false;
	}

	// Cut of the linefeed (and everything following it) if it's present.
	char * const linefeed = strchr(programPath, '\n');
	if (linefeed)
	{
		*linefeed = '\0';
	}

	// Check to see whether we retrieved any meaning ful result
	if (strlen(programPath) == 0)
	{
		debug(LOG_WARNING, "Could not retrieve full path to \"%s\". This may prevent creation of an extended backtrace.", programCommand);
		return false;
	}

	debug(LOG_WZ, "Found program \"%s\" at path \"%s\"", programCommand, programPath);
	return true;
}
#endif

/**
 * Setup the exception handler responsible for target OS.
 *
 * \param programCommand Command used to launch this program. Only used for POSIX handler.
 */
void setupExceptionHandler(int argc, char * argv[])
{
#if !defined(WZ_OS_MAC)
	// Initialize info required for the debug dumper
	dbgDumpInit(argc, argv);
#endif

#if defined(WZ_OS_WIN)
# if defined(WZ_CC_MINGW)
	ExchndlSetup();
# else
	prevExceptionHandler = SetUnhandledExceptionFilter(windowsExceptionHandler);
# endif // !defined(WZ_CC_MINGW)
#elif defined(WZ_OS_UNIX) && !defined(WZ_OS_MAC)
	const char * const programCommand = argv[0];

	// Get full path to this program. Needed for gdb to find the binary.
	programIsAvailable = fetchProgramPath(programPath, sizeof(programPath), programCommand);

	// Get full path to 'gdb'
	gdbIsAvailable = fetchProgramPath(gdbPath, sizeof(gdbPath), "gdb");

	sysInfoValid = (uname(&sysInfo) == 0);

	time_t currentTime = time(NULL);
	sstrcpy(executionDate, ctime(&currentTime));

	snprintf(programPID, sizeof(programPID), "%i", getpid());

	setFatalSignalHandler(posixExceptionHandler);
#endif // WZ_OS_*
}
