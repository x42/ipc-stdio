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

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef BUFSIZ
#define BUFSIZ 8192
#endif

struct IPCMsgBuf {
	long mtype;
	char mtext[BUFSIZ];
};

void*
rx_thread (void* arg)
{
	int msqid = *((int*)arg);
	while (true) {
		ssize_t          rv;
		struct IPCMsgBuf rxbuf;
		if (-1 == (rv = msgrcv (msqid, (void*)&rxbuf, BUFSIZ, 1, MSG_NOERROR))) {
			fprintf (stderr, "ERROR: msgrcv failed. %d: %s\n", errno, strerror (errno));
			break;
		}
		for (int i = 0; i < rv; ++i) {
			putchar (rxbuf.mtext[i]);
		}
		fflush (stdout);
	}
	kill (0, SIGHUP);
	pthread_exit (0);
	return 0;
}

static void
usage ()
{
	// help2man compatible format (standard GNU help-text)
	printf ("ipc-client - connect to a stdio-ipc-server\n\n");
	printf ("Usage: ipc-client [ OPTIONS ]\n\n");
	printf ("Options:\n"
	        "  -h, --help               display this help and exit\n"
	        "  -q, --queuename <path>   specify IPC path of server\n"
	        "  -V, --version            print version information and exit\n"
	        "\n\n"
	        /* 345678901234567890123456789012345678901234567890123456789012345678901234567890 */
	        "Connect to a ipc-server(1), and bi-directionally forward stdin and stdout.\n"
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
	char* qname = "/tmp";

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

	key_t key_rx = ftok (qname, 'a');
	key_t key_tx = ftok (qname, 'b');

	if (key_rx == -1 || key_tx == -1) {
		fprintf (stderr, "Cannot create IPC keys. Error (%d): %s\n", errno, strerror (errno));
		return -1;
	}

	int msqrx = msgget (key_rx, 0);
	int msqtx = msgget (key_tx, 0);

	if (msqrx == -1 || msqtx == -1) {
		fprintf (stderr, "Cannot open IPC queues. Error (%d): %s\n", errno, strerror (errno));
		return -1;
	}

	pthread_t xet;
	if (pthread_create (&xet, NULL, rx_thread, (void*)&msqrx)) {
		fprintf (stderr, "Cannot start background read thread.\n");
		return -2;
	}

	siginterrupt (SIGHUP, 1);

	struct IPCMsgBuf txbuf;
	txbuf.mtype = 1;
	while (fgets (txbuf.mtext, BUFSIZ, stdin) != NULL) {
		size_t l;
		if ((l = strlen (txbuf.mtext)) == 0) {
			break;
		}
		int retry = 10;
		while (--retry && msgsnd (msqtx, &txbuf, l, IPC_NOWAIT) == -1) {
			if (errno != EAGAIN) {
				break;
			}
			usleep (50000);
		}
		if (0 == retry) {
			fprintf (stderr, "msgsnd failed. Error = %d: %s\n", errno, strerror (errno));
			break;
		}
	}

	pthread_cancel (xet);
	pthread_join (xet, NULL);
	return 0;
}
