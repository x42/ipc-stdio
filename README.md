IPC-STDIO
=========

This is a inter process communication (IPC) client/server tool to forward standard i/o
between applications.

It is intended to "daemonize" a process that requires stdio, and allow to re-connect
to the process at a later time.

Installation
------------

```bash
 git clone git://github.com/x42/ipc-stdio.git
 cd ipc-stdio
 make
 sudo make install
```

The makefile honors `CFLAGS`, `LDFLAGS`, `DESTDIR` and `PREFIX` variables.
e.g. `make install PREFIX=/usr` and also supports `uninstall` target as well as
individual `[un]install-bin`, `[un]install-man` targets.


Example Usage
-------------

Start a server-process and pass it a command to run.

```bash
 ipc-server /bin/cat
```

The connect to it:

```bash
 ipc-client
```

In this case everything you write to the client is passed to the server
and echoed back. Use ctrl+D (EOF) to terminate the client.
The server keeps running.
