#include "linebuf.h"

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <err.h>
#include <poll.h>
#include <sys/wait.h>
#include <fcntl.h>

struct proc
{
	struct pollfd	*pfd;
	const char	*cmd;
	int		 idx;
	pid_t		 pid;
	int		 observe_fd;
	struct linebuf	*lb;
};

#ifndef ARRLEN
#define ARRLEN(_x) (sizeof((_x)) / sizeof((_x)[0]))
#endif

#ifndef INFTIM
#define INFTIM -1
#endif

#define READ_BUF_SZ	256

static void discard_spaces(char *, char *);
static int fork_cmd(const char *, int, struct pollfd *, struct proc *);
static int read_fd(int, struct proc *);

static void
discard_spaces(char *begin, char *p)
{
	while (p != begin && isspace(*--p))
		*p = '\0';
}

static int
fork_cmd(const char *cmd, int stdin_fd, struct pollfd *pollfd,
    struct proc *proc)
{
	int		 observe_pipe[2], cmd_pipe[2];
	static int	 idx;

	if (pipe(cmd_pipe) != 0)
		err(1, "pipe (cmd)");

	/*
	 * Clear the close-on-exec flag that was set earlier.
	 */
	fcntl(stdin_fd, F_SETFD, 0);

	if ((proc->pid = fork()) < 0)
		err(1, "fork");
	else if (proc->pid == 0) {
		close(cmd_pipe[0]);
		if (dup2(stdin_fd, 0) != 0)
			err(1, "dup stdin");
		if (dup2(cmd_pipe[1], 1) != 1)
			err(1, "dup stdout");
		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		warn("execl %s", cmd);
		_exit(127);
	}

	close(cmd_pipe[1]);
	close(stdin_fd);

	if (pipe2(observe_pipe, O_CLOEXEC) != 0)
		err(1, "pipe (observe)");

	stdin_fd = observe_pipe[0];
	proc->observe_fd = observe_pipe[1];
	pollfd->fd = cmd_pipe[0];

	pollfd->events = POLLIN;
	proc->pfd = pollfd;

	proc->idx = ++idx;
	proc->cmd = cmd;
	proc->lb = linebuf_create();

	/*
	 * Return the command's stdout fd that can be passed to the next
	 * command as stdin fd.
	 */
	return stdin_fd;
}

static int
read_fd(int fd, struct proc *proc)
{
	int	 ret, n, n2;
	int	 status;
	char	 buf[READ_BUF_SZ], *p;
	char	*s;

	ret = n = read(fd, buf, sizeof(buf) - 1);
	if (n <= 0) {
		if (n < 0)
			warn("read");
		printf("%d-", proc->idx);
		waitpid(proc->pid, &status, 0);

		if (WIFEXITED(status))
			printf("exit(%d)\n", WEXITSTATUS(status));
		else if (WIFSIGNALED(status))
			printf("signal(%d)\n", WTERMSIG(status));
		else
			putchar('\n');

		if (proc->observe_fd != -1)
			close(proc->observe_fd);
		close(fd);

		linebuf_free(proc->lb);
	} else {
		if (proc->observe_fd != -1)
			write(proc->observe_fd, buf, n);
		buf[n] = '\0';

		p = buf;
		while ((n2 = linebuf_fill(proc->lb, p, n)) > 0) {
			p += n2;
			n -= n2;
			while ((s = linebuf_read(proc->lb)) != NULL)
				printf("%d: %s\n", proc->idx, s);
		}
	}

	return ret;
}

int
main(int argc, char *argv[])
{
	char			*p, *cmd;
	static struct pollfd	 pfds[16];
	static struct proc	 procs[16];
	struct pollfd		*pfd = NULL;
	struct proc		*proc = NULL;
	int			 nfds;
	int			 nready;
	int			 i;
	int			 n;
	int			 stdin_fd;

	if (argc < 2) {
		fprintf(stderr, "%s 'cmd1 | cmd2 | ...'\n", argv[0]);
		return 1;
	}

	/*
	 * Read the pipeline from command line and then perform the
	 * following algorithm:
	 *
	 *   1. Read command's stdout_fd
	 *   2. Write it to observe_fd (observe_pipe's write side)
	 *   3. Make stdin_fd (which is observe_pipe's read side) to be
	 *      next cmd's stdin
	 *   4. Repeat
	 */
	nfds = 0;
	stdin_fd = 0;
	p = cmd = argv[1];
	while ((p = strchr(cmd, '|')) != NULL) {
		discard_spaces(cmd, p);
		*p++ = '\0';

		stdin_fd = fork_cmd(cmd, stdin_fd, &pfds[nfds], &procs[nfds]);
		nfds++;

		while (isspace(*p))
			p++;
		cmd = p;
	}
	stdin_fd = fork_cmd(cmd, stdin_fd, &pfds[nfds], &procs[nfds]);
	nfds++;

	/*
	 * Because this is the last command, we need to close the pipes
	 * connecting to the next command.
	 */
	close(procs[nfds-1].observe_fd);
	procs[nfds-1].observe_fd = -1;
	close(stdin_fd);

	while (nfds > 0) {
		nready = poll(pfds, nfds, INFTIM);
		if (nready <= 0)
			break;
		for (i = 0; i < nfds; i++) {
			pfd = &pfds[i];
			proc = &procs[i];
			if (pfd->revents & (POLLIN|POLLHUP)) {
				if ((n = read_fd(pfd->fd, proc)) <= 0) {
					nfds--;
					if (i < nfds && nfds > 0) {
						memmove(&pfds[i], &pfds[i+1],
						    (nfds - i) *
						    sizeof(struct pollfd));
						memmove(&procs[i], &procs[i+1],
						    (nfds - i) *
						    sizeof(struct proc));
					}
				}
			}
		}
	}
	if (nready == -1)
		err(1, "poll");

	return 0;
}
