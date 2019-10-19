/*
 * Copyright (C) 2019 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef BUFSIZ
#define BUFSIZ 8192
#endif

#define SHELL_PATH "/bin/sh"
#define SHELL_NAME "sh"

struct IPCMsgBuf {
	long mtype;
	char mtext[BUFSIZ];
};

static int msqtx = -1;
static int msqrx = -1;

pid_t     pid       = 0;
int       nicelevel = 0;
pthread_t thread_id_tt;
bool      thread_active;

static int pok[2];
static int pin[2];
static int pout[2];

volatile int    terminated = 0;
pthread_mutex_t write_lock;

static int
open_ipcmsg_ctrl (const char* queuename)
{
	key_t key_tx = ftok (queuename, 'a');
	key_t key_rx = ftok (queuename, 'b');

	if (key_rx == -1 || key_tx == -1) {
		fprintf (stderr, "Cannot create IPC keys. Error (%d): %s\n", errno, strerror (errno));
		return -1;
	}

	msqrx = msgget (key_rx, IPC_CREAT | S_IRUSR | S_IWUSR);
	msqtx = msgget (key_tx, IPC_CREAT | S_IRUSR | S_IWUSR);

	if (msqrx == -1 || msqtx == -1) {
		fprintf (stderr, "Cannot create IPC queues. Error (%d): %s\n", errno, strerror (errno));
		return -1;
	}
	return 0;
}

static void
close_ipc_queues ()
{
	if (msqtx >= 0) {
		msgctl (msqtx, IPC_RMID, NULL);
	}
	if (msqrx >= 0) {
		msgctl (msqrx, IPC_RMID, NULL);
	}
}

static int
reply (char* msg, size_t len)
{
	if (len >= BUFSIZ) {
		return -1;
	}
	struct IPCMsgBuf txbuf;
	txbuf.mtype = 1;
	memcpy (txbuf.mtext, msg, len);
	if (msgsnd (msqtx, (const void*)&txbuf, len, IPC_NOWAIT) == -1) {
		fprintf (stderr, "Cannot sent message. Error (%d): %s\n", errno, strerror (errno));
		return -1;
	}
	return 0;
}

static void*
output_interposer ()
{
	int           rfd = pout[0];
	char          buf[BUFSIZ];
	ssize_t       r;
	unsigned long l = 1;

	ioctl (rfd, FIONBIO, &l); // set non-blocking I/O

	for (; fcntl (rfd, F_GETFL) != -1;) {
		r = read (rfd, buf, BUFSIZ - 1);
		if (r < 0 && (errno == EINTR || errno == EAGAIN)) {
			fd_set         rfds;
			struct timeval tv;
			FD_ZERO (&rfds);
			FD_SET (rfd, &rfds);
			tv.tv_sec  = 0;
			tv.tv_usec = 10000;
			int rv     = select (1, &rfds, NULL, NULL, &tv);
			if (rv == -1) {
				break;
			}
			continue;
		}
		if (r <= 0) {
			break;
		}
		buf[r] = 0;
		if (reply (buf, r)) {
			break;
		}
	}
	terminated = 1;
	pthread_exit (0);
}

static void
close_fd (int* fdx)
{
	int fd = *fdx;
	if (fd >= 0) {
		close (fd);
	}
	*fdx = -1;
}

size_t
write_to_stdin (const void* data, size_t bytes)
{
	ssize_t r;
	size_t  c;
	pthread_mutex_lock (&write_lock);

	c = 0;
	while (c < bytes) {
		for (;;) {
			r = write (pin[1], &((const char*)data)[c], bytes - c);
			if (r < 0 && (errno == EINTR || errno == EAGAIN)) {
				sleep (1);
				continue;
			}
			if ((size_t)r != (bytes - c)) {
				pthread_mutex_unlock (&write_lock);
				return c;
			}
			break;
		}
		c += r;
	}
	fsync (pin[1]);
	pthread_mutex_unlock (&write_lock);
	return c;
}

void
close_stdin ()
{
	if (pin[1] < 0) {
		return;
	}
	close_fd (&pin[0]);
	close_fd (&pin[1]);
	close_fd (&pout[0]);
	close_fd (&pout[1]);
}

static int
_wait (int options)
{
	int status = 0;
	int ret;

	if (pid == 0)
		return -1;

	ret = waitpid (pid, &status, options);

	if (ret == pid) {
		if (WEXITSTATUS (status) || WIFSIGNALED (status)) {
			pid = 0;
		}
	} else {
		if (ret != 0) {
			if (errno == ECHILD) {
				/* no currently running children, reset pid */
				pid = 0;
			}
		} /* else the process is still running */
	}
	return status;
}

static void
terminate ()
{
	pthread_mutex_lock (&write_lock);

	/* close stdin in an attempt to get the child to exit cleanly.  */
	close_stdin ();

	if (pid) {
		usleep (200000);
		sched_yield ();
		_wait (WNOHANG);
	}

	/* if pid is non-zero, the child task is still executing (i.e. it did
	 * not exit in response to stdin being closed). try to kill it.
	 */

	if (pid) {
		kill (pid, SIGTERM);
		usleep (250000);
		sched_yield ();
		_wait (WNOHANG);
	}

	/* if pid is non-zero, the child task is STILL executing after being
	 * sent SIGTERM. Act tough ... send SIGKILL
	 */

	if (pid) {
		fprintf (stderr, "Process is still running! trying SIGKILL\n");
		kill (pid, SIGKILL);
	}

	_wait (0);

	if (thread_active) {
		pthread_join (thread_id_tt, NULL);
	}
	thread_active = false;
	assert (pid == 0);
	pthread_mutex_unlock (&write_lock);
}

static int
start (char* const* argp)
{
	if (pipe (pin) < 0 || pipe (pout) < 0 || pipe (pok) < 0) {
		return -1;
	}
	int r = fork ();

	if (r > 0) {
		/* main */
		pid = r;

		/* check if execve was successful. */
		close_fd (&pok[1]);
		char buf;
		for (;;) {
			ssize_t n = read (pok[0], &buf, 1);
			if (n == 1) {
				/* child process returned from execve */
				pid = 0;
				close_fd (&pok[0]);
				close_fd (&pok[1]);
				close_fd (&pin[1]);
				close_fd (&pin[0]);
				close_fd (&pout[1]);
				close_fd (&pout[0]);
				return -3;
			} else if (n == -1) {
				if (errno == EAGAIN || errno == EINTR) {
					continue;
				}
			}
			break;
		}

		close_fd (&pok[0]);
		/* child started successfully */

		close_fd (&pok[0]);
		/* child started successfully */

		close_fd (&pout[1]);
		close_fd (&pin[0]);

		int rv        = pthread_create (&thread_id_tt, NULL, output_interposer, NULL);
		thread_active = true;

		if (rv) {
			thread_active = false;
			terminate ();
			return -2;
		}
		return 0; /* all systems go - return to main */
	}

	/* child process - exec external process */
	close_fd (&pok[0]);
	fcntl (pok[1], F_SETFD, FD_CLOEXEC);

	close_fd (&pin[1]);
	if (pin[0] != STDIN_FILENO) {
		dup2 (pin[0], STDIN_FILENO);
	}
	close_fd (&pin[0]);
	close_fd (&pout[0]);
	if (pout[1] != STDOUT_FILENO) {
		dup2 (pout[1], STDOUT_FILENO);
	}

	/* merge STDERR into output */
	if (pout[1] != STDERR_FILENO) {
		dup2 (pout[1], STDERR_FILENO);
	}

	if (pout[1] != STDOUT_FILENO && pout[1] != STDERR_FILENO) {
		close_fd (&pout[1]);
	}

	if (nicelevel != 0) {
		nice (nicelevel);
	}

	execve (argp[0], argp, environ);

	/* if we reach here something went wrong.. */
	char buf = 0;
	(void)write (pok[1], &buf, 1);
	close_fd (&pok[1]);
	exit (EXIT_FAILURE);
	return -1;
}

static void
catchsig (int sig)
{
	fprintf (stderr, "Caught signal, shutting down.\n");
	terminate ();
}

static void
doit ()
{
	signal (SIGINT, catchsig);

	while (true) {
		if (terminated) {
			fprintf (stderr, "Child app rerminated\n");
			break;
		}
		ssize_t          rv;
		struct IPCMsgBuf rxbuf;
		if (-1 == (rv = msgrcv (msqrx, (void*)&rxbuf, BUFSIZ, 1, MSG_NOERROR | IPC_NOWAIT))) {
			if (errno == ENOMSG) {
				usleep (100000);
				continue;
			}
			fprintf (stderr, "msgrcv() failed. Error (%d): %s\n", errno, strerror (errno));
			break;
		}
		write_to_stdin (rxbuf.mtext, rv);
	}
}

static void
init_globals ()
{
	pthread_mutex_init (&write_lock, NULL);
	thread_active = false;
	pid           = 0;
	pin[1]        = -1;
	nicelevel     = 0;
}

static void
usage ()
{
	// help2man compatible format (standard GNU help-text)
	printf ("ipc-server - wrap stdio of a child process \n\n");
	printf ("Usage: ipc-server [ OPTIONS ] <command>\n\n");
	printf ("Options:\n"
	        "  -h, --help               display this help and exit\n"
	        "  -q, --queuename <path>   specify IPC path identifier to use\n"
	        "  -V, --version            print version information and exit\n"
	        "\n\n"
	        /* 345678901234567890123456789012345678901234567890123456789012345678901234567890 */
	        "Launch a child process and expose its stdin and stdout/stderr via IPC to a\n"
	        "ipc-client. The process continues running, even if the IPC client disconnects.\n"
	        "\n"
	        "The queue-name must point to an existing file. The file itself is irrelevant,\n"
	        "it is only used as identifier. The default is '/tmp'.\n"
	        "\n"
	        "The <command> must be an absolute path to a binary to execute.\n"
	        "\n"
	        "Examples:\n"
	        "stdio-ipc /bin/cat\n"
	        "\n"
	        "Report bugs to Robin Gareus <robin@gareus.org>\n"
	        "Website and manual: <https://github.com/x42/ipc-stdio>\n");
	exit (EXIT_SUCCESS);
}

static struct option const long_options[] = {
	{ "help", no_argument, 0, 'h' },
	{ "queuename", required_argument, 0, 'q' },
	{ "version", no_argument, 0, 'V' },
	{ NULL, 0, NULL, 0 }
};

int
main (int argc, char** argv)
{
	init_globals ();

	bool  use_shell = false;
	char* qname     = "/tmp";

	int c;
	while ((c = getopt_long (argc, argv,
	                         "q:" /* queuename */
	                         "h"  /* help */
	                         "V", /* version */
	                         long_options, (int*)0)) != EOF) {
		switch (c) {
			case 'q':
				qname = optarg;
				break;
			case 'V':
				printf ("stdioipc version %s\n\n", VERSION);
				printf ("Copyright (C) GPL 2019 Robin Gareus <robin@gareus.org>\n");
				exit (EXIT_SUCCESS);
			case 'h':
				usage ();
			default:
				fprintf (stderr, "Error: unrecognized option. See --help for usage information.\n");
				exit (EXIT_FAILURE);
				break;
		}
	}

	if (optind + 1 != argc) {
		fprintf (stderr, "Error: missing command. See --help for usage information.\n");
		exit (EXIT_FAILURE);
	}

	char* cmd = argv[optind];

	if (open_ipcmsg_ctrl (qname)) {
		close_ipc_queues ();
		return -1;
	}

	int    argn = 0;
	char** argp;

	if (use_shell) {
		argp         = (char**)calloc (4, sizeof (char*));
		argp[argn++] = strdup (SHELL_PATH);
		argp[argn++] = strdup ("-c");
		argp[argn++] = strdup (cmd);
		argp[argn]   = 0;
	} else {
		argp         = (char**)calloc (2, sizeof (char*));
		argp[argn++] = strdup (cmd);
		argv[argc]   = 0;
	}

	int rv = start (argp);
	switch (rv) {
		case 0:
			doit ();
			break;
		default:
			fprintf (stderr, "Failed to start child process (%d).\n", rv);
	}

	for (argn = 0; argp[argn]; ++argn) {
		free (argp[argn]);
	}
	free (argp);

	terminate ();
	close_ipc_queues ();
	unlink (qname);
	return rv;
}
