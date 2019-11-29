/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Priviledge Separation for dhcpcd
 * Copyright (c) 2006-2019 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * The current design is this:
 * Spawn a priv process to carry out privileged actions and
 * spawning unpriv process to initate network connections such as BPF
 * or address specific listener.
 * Spawn an unpriv process to send/receive common network data.
 * Then drop all privs and start running.
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef AF_LINK
#include <net/if_dl.h>
#endif

#include <assert.h>
#include <pwd.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arp.h"
#include "common.h"
#include "control.h"
#include "dhcp.h"
#include "dhcp6.h"
#include "eloop.h"
#include "ipv6nd.h"
#include "logerr.h"
#include "privsep.h"

#ifdef HAVE_UTIL_H
#include <util.h>
#endif

pid_t
ps_dostart(struct dhcpcd_ctx *ctx,
    pid_t *priv_pid, int *priv_fd,
    void (*recv_msg)(void *), void (*recv_unpriv_msg),
    void *recv_ctx, int (*callback)(void *), void (*signal_cb)(int, void *),
    unsigned int flags)
{
	struct passwd *pw;
	int stype;
	int fd[2];
	pid_t pid;

	ctx->options |= DHCPCD_PRIVSEP;

	if (!(flags & PSF_DROPPRIVS)) {
		pw = NULL;
		goto create_sp;
	}

	errno = 0;
	if ((pw = getpwnam(DHCPCD_USER)) == NULL) {
		if (errno == 0) {
			if (ctx == recv_ctx) /* Only log the once. */
				logerrx("no such user %s", DHCPCD_USER);
		} else
			logerr("getpwnam");
		ctx->options &= ~DHCPCD_PRIVSEP;
		return -1;
	}

	if (priv_pid == NULL) {
		gid_t gid = (gid_t)-1;

		/* Main process - change ownership of stuff we need to
		 * drop at exit. */
		if (pw != NULL) {
			if (chown(ctx->pidfile, pw->pw_uid, gid) == -1)
				logerr("chown `%s'", ctx->pidfile);
			if (chown(DBDIR, pw->pw_uid, gid) == -1)
				logerr("chown `%s'", DBDIR);
			if (chown(RUNDIR, pw->pw_uid, gid) == -1)
				logerr("chown `%s'", RUNDIR);
			if (ctx->options & DHCPCD_MASTER) {
				if (chown(ctx->control_sock,
				    pw->pw_uid, gid) == -1)
					logerr("chown `%s'", ctx->control_sock);
				if (chown(UNPRIVSOCKET, pw->pw_uid, gid) == -1)
					logerr("chown `%s'", UNPRIVSOCKET);
			}
		}
		goto dropprivs;
	}

create_sp:
	stype = SOCK_CLOEXEC | SOCK_NONBLOCK;
	if (socketpair(AF_UNIX, SOCK_DGRAM | stype, 0, fd) == -1) {
		logerr("socketpair");
		return -1;
	}

	switch (pid = fork()) {
	case -1:
		logerr("fork");
		return -1;
	case 0:
		*priv_fd = fd[1];
		close(fd[0]);
		break;
	default:
		*priv_pid = pid;
		*priv_fd = fd[0];
		close(fd[1]);
		if (recv_unpriv_msg != NULL &&
		    eloop_event_add(ctx->eloop, *priv_fd,
		    recv_unpriv_msg, recv_ctx) == -1)
		{
			logerr("%s: eloop_event_add", __func__);
			return -1;
		}
		return pid;
	}

	ctx->options |= DHCPCD_UNPRIV | DHCPCD_FORKED;
	control_close(ctx);
	pidfile_clean();
	eloop_clear(ctx->eloop);

	if (eloop_signal_set_cb(ctx->eloop,
	    dhcpcd_signals, dhcpcd_signals_len, signal_cb, ctx) == -1)
	{
		logerr("%s: eloop_signal_set_cb", __func__);
		goto errexit;
	}
	if (eloop_signal_mask(ctx->eloop, &ctx->sigset) == -1) {
		logerr("%s: eloop_signal_mask", __func__);
		goto errexit;
	}

	if (eloop_event_add(ctx->eloop, *priv_fd, recv_msg, recv_ctx) == -1)
	{
		logerr("%s: eloop_event_add", __func__);
		goto errexit;
	}

	/* We are not root */
	if (priv_fd != &ctx->ps_root_fd) {
		ps_freeprocesses(ctx, recv_ctx);
		close(ctx->ps_root_fd);
		ctx->ps_root_fd = -1;
	}

	if (priv_fd != &ctx->ps_inet_fd) {
		close(ctx->ps_inet_fd);
		ctx->ps_inet_fd = -1;
	}

	if (callback(recv_ctx) == -1)
		goto errexit;

	if (pw == NULL)
		return 0;

	if (!(flags & PSF_DROPPRIVS)) {
		if (chdir(pw->pw_dir) == -1)
			logerr("%s: chdir `%s'", __func__, pw->pw_dir);
		return 0;
	}

	if (chroot(pw->pw_dir) == -1)
		logerr("%s: chroot `%s'", __func__, pw->pw_dir);

dropprivs:
	if (setgroups(1, &pw->pw_gid) == -1 ||
	     setgid(pw->pw_gid) == -1 ||
	     setuid(pw->pw_uid) == -1)
		logerr("failed to drop privileges");

	return 0;

errexit:
	/* Failure to start root or inet processes is fatal. */
	if (priv_fd == &ctx->ps_root_fd || priv_fd == &ctx->ps_inet_fd)
		ps_sendcmd(ctx, *priv_fd, PS_STOP, 0, NULL, 0);
	shutdown(*priv_fd, SHUT_RDWR);
	*priv_fd = -1;
	return -1;
}

int
ps_dostop(struct dhcpcd_ctx *ctx, pid_t *pid, int *fd)
{
	int status;

#ifdef PRIVSEP_DEBUG
	logdebugx("%s: pid %d fd %d", __func__, *pid, *fd);
#endif
	if (*pid == 0)
		return 0;
	eloop_event_delete(ctx->eloop, *fd);
	if (ps_sendcmd(ctx, *fd, PS_STOP, 0, NULL, 0) == -1 &&
	    errno != ECONNRESET)
		logerr(__func__);
	if (shutdown(*fd, SHUT_RDWR) == -1 && errno != ENOTCONN)
		logerr(__func__);
	close(*fd);
	*fd = -1;
	/* We won't have permission for all processes .... */
#if 0
	if (kill(*pid, SIGTERM) == -1)
		logerr(__func__);
#endif
	status = 0;
	/* Wait for the process to finish */
	while (waitpid(*pid, &status, 0) == -1) {
		if (errno != EINTR) {
			logerr("%s: waitpid", __func__);
			status = 0;
			break;
		}
#ifdef PRIVSEP_DEBUG
		else
			logerr("%s: waitpid ", __func__);
#endif
	}
	*pid = 0;

#ifdef PRIVSEP_DEBUG
	logdebugx("%s: status %d", __func__, status);
#endif

	return status;
}

int
ps_start(struct dhcpcd_ctx *ctx)
{
	pid_t pid;

	TAILQ_INIT(&ctx->ps_processes);

	switch (pid = ps_root_start(ctx)) {
	case -1:
		return -1;
	case 0:
		return 0;
	default:
		logdebugx("spawned privileged actioneer on PID %d", pid);
	}

	/* No point in spawning the generic network listener if we're
	 * not going to use it. */
	if (!(ctx->options & (DHCPCD_MASTER | DHCPCD_IPV6RS)))
		goto dropprivs;

	switch (pid = ps_inet_start(ctx)) {
	case -1:
		if (errno == ENXIO)
			return 0;
		return -1;
	case 0:
		return 0;
	default:
		logdebugx("spawned network proxy on PID %d", pid);
	}

dropprivs:
	/* Drop privs now. */
	ps_dostart(ctx, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	    PSF_DROPPRIVS);

	return 1;
}

int
ps_stop(struct dhcpcd_ctx *ctx)
{
	int r, ret = 0;

	if (ctx->options & DHCPCD_FORKED || ctx->eloop == NULL)
		return 0;

	r = ps_inet_stop(ctx);
	if (r != 0)
		ret = r;
	r = ps_root_stop(ctx);
	if (r != 0)
		ret = r;

	ctx->options &= ~DHCPCD_PRIVSEP;
	return ret;
}

void
ps_freeprocess(struct ps_process *psp)
{
#ifdef INET
	struct ipv4_state *istate = IPV4_STATE(&psp->psp_ifp);

	if (istate != NULL) {
		free(istate->buffer);
		free(istate);
	}
#endif

	TAILQ_REMOVE(&psp->psp_ctx->ps_processes, psp, next);
	if (psp->psp_fd != -1)
		close(psp->psp_fd);
	free(psp);
}

static void
ps_free(struct dhcpcd_ctx *ctx)
{
	struct ps_process *psp;
	bool stop = ctx->ps_root_pid == getpid();

	while ((psp = TAILQ_FIRST(&ctx->ps_processes)) != NULL) {
		if (stop)
			ps_dostop(ctx, &psp->psp_pid, &psp->psp_fd);
		ps_freeprocess(psp);
	}
}

int
ps_unrollmsg(struct msghdr *msg, struct ps_msghdr *psm,
    const void *data, size_t len)
{
	uint8_t *datap, *namep, *controlp;

	namep = UNCONST(data);
	controlp = namep + psm->ps_namelen;
	datap = controlp + psm->ps_controllen;

	if (psm->ps_namelen != 0) {
		if (psm->ps_namelen > len) {
			errno = EINVAL;
			return -1;
		}
		msg->msg_name = namep;
		len -= psm->ps_namelen;
	} else
		msg->msg_name = NULL;
	msg->msg_namelen = psm->ps_namelen;

	if (psm->ps_controllen != 0) {
		if (psm->ps_controllen > len) {
			errno = EINVAL;
			return -1;
		}
		msg->msg_control = controlp;
		len -= psm->ps_controllen;
	} else
		msg->msg_control = NULL;
	msg->msg_controllen = psm->ps_controllen;

	if (len != 0) {
		msg->msg_iovlen = 1;
		msg->msg_iov[0].iov_base = datap;
		msg->msg_iov[0].iov_len = len;
	} else {
		msg->msg_iovlen = 0;
		msg->msg_iov[0].iov_base = NULL;
		msg->msg_iov[0].iov_len = 0;
	}
	return 0;
}


ssize_t
ps_sendpsmmsg(struct dhcpcd_ctx *ctx, int fd,
    struct ps_msghdr *psm, const struct msghdr *msg)
{
	assert(msg == NULL || msg->msg_iovlen == 1);

	struct iovec iov[] = {
		{ .iov_base = UNCONST(psm), .iov_len = sizeof(*psm) },
		{ .iov_base = NULL, },	/* name */
		{ .iov_base = NULL, },	/* control */
		{ .iov_base = NULL, },	/* payload */
	};
	int iovlen = __arraycount(iov);
	ssize_t len;

	if (msg != NULL) {
		struct iovec *iovp = &iov[1];

		assert(msg->msg_iovlen == 1);

		psm->ps_namelen = msg->msg_namelen;
		psm->ps_controllen = (socklen_t)msg->msg_controllen;

		iovp->iov_base = msg->msg_name;
		iovp->iov_len = msg->msg_namelen;
		iovp++;
		iovp->iov_base = msg->msg_control;
		iovp->iov_len = msg->msg_controllen;
		iovp++;
		iovp->iov_base = msg->msg_iov[0].iov_base;
		iovp->iov_len = msg->msg_iov[0].iov_len;
		iovlen = __arraycount(iov);
	} else
		iovlen = 1;

	len = writev(fd, iov, iovlen);
#ifdef PRIVSEP_DEBUG
	logdebugx("%s: %zd", __func__, len);
#endif
	if ((len == -1 || len == 0) && ctx->options & DHCPCD_FORKED)
		eloop_exit(ctx->eloop, len == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
	return len;
}

ssize_t
ps_sendpsmdata(struct dhcpcd_ctx *ctx, int fd,
    struct ps_msghdr *psm, const void *data, size_t len)
{
	struct iovec iov[] = {
		{ .iov_base = UNCONST(data), .iov_len = len },
	};
	struct msghdr msg = {
		.msg_iov = iov, .msg_iovlen = 1,
	};

	return ps_sendpsmmsg(ctx, fd, psm, &msg);
}


ssize_t
ps_sendmsg(struct dhcpcd_ctx *ctx, int fd, uint8_t cmd, unsigned long flags,
    const struct msghdr *msg)
{
	assert(msg->msg_iovlen == 1);

	struct ps_msghdr psm = {
		.ps_cmd = cmd,
		.ps_flags = flags,
		.ps_namelen = msg->msg_namelen,
		.ps_controllen = (socklen_t)msg->msg_controllen,
		.ps_datalen = msg->msg_iov[0].iov_len,
	};

	return ps_sendpsmmsg(ctx, fd, &psm, msg);
}

ssize_t
ps_sendcmd(struct dhcpcd_ctx *ctx, int fd, uint8_t cmd, unsigned long flags,
    const void *data, size_t len)
{
	struct ps_msghdr psm = {
		.ps_cmd = cmd,
		.ps_flags = flags,
	};
	struct iovec iov[] = {
		{ .iov_base = UNCONST(data), .iov_len = len }
	};
	struct msghdr msg = {
		.msg_iov = iov, .msg_iovlen = 1,
	};

	return ps_sendpsmmsg(ctx, fd, &psm, &msg);
}

static ssize_t
ps_sendcmdmsg(int fd, uint8_t cmd, const struct msghdr *msg)
{
	struct ps_msghdr psm = { .ps_cmd = cmd };
	uint8_t data[PS_BUFLEN], *p = data;
	struct iovec iov[] = {
		{ .iov_base = &psm, .iov_len = sizeof(psm) },
		{ .iov_base = data, .iov_len = 0 },
	};
	size_t dl = sizeof(data);

	if (msg->msg_namelen != 0) {
		if (msg->msg_namelen > dl)
			goto nobufs;
		psm.ps_namelen = msg->msg_namelen;
		memcpy(p, msg->msg_name, msg->msg_namelen);
		p += msg->msg_namelen;
		dl -= msg->msg_namelen;
	}

	if (msg->msg_controllen != 0) {
		if (msg->msg_controllen > dl)
			goto nobufs;
		psm.ps_controllen = (socklen_t)msg->msg_controllen;
		memcpy(p, msg->msg_control, msg->msg_controllen);
		p += msg->msg_controllen;
		dl -= msg->msg_controllen;
	}

	psm.ps_datalen = msg->msg_iov[0].iov_len;
	if (psm.ps_datalen > dl)
		goto nobufs;

	iov[1].iov_len = psm.ps_namelen + psm.ps_controllen + psm.ps_datalen;
	if (psm.ps_datalen != 0)
		memcpy(p, msg->msg_iov[0].iov_base, psm.ps_datalen);
	return writev(fd, iov, __arraycount(iov));

nobufs:
	errno = ENOBUFS;
	return -1;
}

ssize_t
ps_recvmsg(struct dhcpcd_ctx *ctx, int rfd, uint8_t cmd, int wfd)
{
	struct sockaddr_storage ss = { .ss_family = AF_UNSPEC };
	uint8_t controlbuf[sizeof(struct sockaddr_storage)] = { 0 };
	uint8_t databuf[64 * 1024];
	struct iovec iov[] = {
	    { .iov_base = databuf, .iov_len = sizeof(databuf) }
	};
	struct msghdr msg = {
		.msg_name = &ss, .msg_namelen = sizeof(ss),
		.msg_control = controlbuf, .msg_controllen = sizeof(controlbuf),
		.msg_iov = iov, .msg_iovlen = 1,
	};

	ssize_t len = recvmsg(rfd, &msg, 0);
#ifdef PRIVSEP_DEBUG
	logdebugx("%s: recv fd %d, %zd bytes", __func__, rfd, len);
#endif
	if ((len == -1 || len == 0) && ctx->options & DHCPCD_FORKED) {
		eloop_exit(ctx->eloop, len == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
		return len;
	}

	iov[0].iov_len = (size_t)len;
	len = ps_sendcmdmsg(wfd, cmd, &msg);
#ifdef PRIVSEP_DEBUG
	logdebugx("%s: send fd %d, %zu bytes", __func__, wfd, len);
#endif
	if ((len == -1 || len == 0) && ctx->options & DHCPCD_FORKED)
		eloop_exit(ctx->eloop, len == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
	return len;
}

ssize_t
ps_recvpsmsg(struct dhcpcd_ctx *ctx, int fd,
    ssize_t (*callback)(void *, struct ps_msghdr *, struct msghdr *),
    void *cbctx)
{
	struct ps_msg psm;
	ssize_t len;
	size_t dlen;
	struct iovec iov[1];
	struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 1 };
	bool stop = false;

	len = read(fd, &psm, sizeof(psm));
#ifdef PRIVSEP_DEBUG
	logdebugx("%s: %zd", __func__, len);
#endif

	if (len == -1 && (errno == ECONNRESET || errno == EBADF))
		len = 0;
	if (len == -1 || len == 0)
		stop = true;
	else {
		dlen = (size_t)len;
		if (dlen < sizeof(psm.psm_hdr)) {
			errno = EINVAL;
			return -1;
		}

		if (psm.psm_hdr.ps_cmd == PS_STOP) {
			stop = true;
			len = 0;
		}
	}

	if (stop) {
#ifdef PRIVSEP_DEBUG
		logdebugx("process %d stopping", getpid());
#endif
		ps_free(ctx);
		eloop_exit(ctx->eloop, len != -1 ? EXIT_SUCCESS : EXIT_FAILURE);
		return len;
	}
	dlen -= sizeof(psm.psm_hdr);

	if (ps_unrollmsg(&msg, &psm.psm_hdr, psm.psm_data, dlen) == -1)
		return -1;

	errno = 0;
	return callback(cbctx, &psm.psm_hdr, &msg);
}

struct ps_process *
ps_findprocess(struct dhcpcd_ctx *ctx, struct ps_id *psid)
{
	struct ps_process *psp;

	TAILQ_FOREACH(psp, &ctx->ps_processes, next) {
		if (memcmp(&psp->psp_id, psid, sizeof(psp->psp_id)) == 0)
			return psp;
	}
	errno = ESRCH;
	return NULL;
}

struct ps_process *
ps_newprocess(struct dhcpcd_ctx *ctx, struct ps_id *psid)
{
	struct ps_process *psp;

	psp = calloc(1, sizeof(*psp));
	if (psp == NULL)
		return NULL;
	psp->psp_ctx = ctx;
	memcpy(&psp->psp_id, psid, sizeof(psp->psp_id));
	TAILQ_INSERT_TAIL(&ctx->ps_processes, psp, next);
	return psp;
}

void
ps_freeprocesses(struct dhcpcd_ctx *ctx, struct ps_process *notthis)
{
	struct ps_process *psp, *psn;

	TAILQ_FOREACH_SAFE(psp, &ctx->ps_processes, next, psn) {
		if (psp == notthis)
			continue;
		ps_freeprocess(psp);
	}
}
