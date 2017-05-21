/*
 * Copyright 2013-2017 Fabian Groffen
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>

#include "relay.h"
#include "router.h"

#ifndef TMPDIR
# define TMPDIR "/tmp"
#endif

#define SOCKFILE ".s.carbon-c-relay"


static char
bindlistenip(listener *lsnr, unsigned int backlog)
{
	int sock;
	int optval;
	struct timeval tv;
	struct addrinfo *resw;
	char buf[128];
	char saddr[INET6_ADDRSTRLEN];
	int err;
	int binderr = 0;
	int curlen_stream = 0;
	int curlen_dgram = 0;
	int sockcur = 0;

	tv.tv_sec = 0;
	tv.tv_usec = 500 * 1000;

	for (resw = lsnr->saddrs; resw != NULL; resw = resw->ai_next) {
		if (resw->ai_family != PF_INET && resw->ai_family != PF_INET6)
			continue;
		if (resw->ai_protocol != IPPROTO_TCP &&
				resw->ai_protocol != IPPROTO_UDP)
			continue;

		saddr_ntop(resw, saddr);
		if (saddr[0] == '\0')
			snprintf(saddr, sizeof(saddr), "(unknown)");

		if ((sock = socket(resw->ai_family, resw->ai_socktype,
						resw->ai_protocol)) < 0)
		{
			logerr("failed to create socket for %s: %s\n",
					saddr, strerror(errno));
			binderr = 1;
			break;
		}
		lsnr->socks[sockcur++] = sock;

		(void) setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		optval = 1;  /* allow takeover */
		(void) setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
				&optval, sizeof(optval));
		if (resw->ai_family == PF_INET6) {
			optval = 1;
			(void) setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
					&optval, sizeof(optval));
		}

		if (bind(sock, resw->ai_addr, resw->ai_addrlen) < 0) {
			logerr("failed to bind on %s%d %s port %s: %s\n",
					resw->ai_protocol == IPPROTO_TCP ? "tcp" : "udp",
					resw->ai_family == PF_INET6 ? 6 : 4,
					saddr, buf, strerror(errno));
			binderr = 1;
			break;
		}

		if (resw->ai_protocol == IPPROTO_TCP) {
			if (listen(sock, backlog) < 0) {
				logerr("failed to listen on tcp%d %s port %s: %s\n",
						resw->ai_family == PF_INET6 ? 6 : 4,
						saddr, buf, strerror(errno));
				binderr = 1;
				break;
			}
			logout("listening on tcp%d %s port %s\n",
					resw->ai_family == PF_INET6 ? 6 : 4, saddr, buf);
		} else {
			logout("listening on udp%d %s port %s\n",
					resw->ai_family == PF_INET6 ? 6 : 4, saddr, buf);
		}
	}
	if (binderr != 0) {
		/* close all opened sockets */
		for (--sockcur; sockcur >= 0; sockcur--)
			close(lsnr->socks[sockcur]);
		return 1;
	}

	return 0;
}

static char
bindlistenunix(listener *lsnr, unsigned int backlog)
{
	struct sockaddr_un server;
	int sock;

#ifndef PF_LOCAL
# define PF_LOCAL PF_UNIX
#endif
	if ((sock = socket(PF_LOCAL, SOCK_STREAM, 0)) < 0) {
		logerr("failed to create socket for %s: %s\n",
				lsnr->ip, strerror(errno));
		return 1;
	}
	lsnr->socks[0] = sock;

	memset(&server, 0, sizeof(struct sockaddr_un));
	server.sun_family = PF_LOCAL;
	strncpy(server.sun_path, lsnr->ip, sizeof(server.sun_path) - 1);

	unlink(lsnr->ip);  /* avoid address already in use */
	if (bind(sock, (struct sockaddr *)&server,
				sizeof(struct sockaddr_un)) < 0)
	{
		logerr("failed to bind for %s: %s\n", lsnr->ip, strerror(errno));
		close(sock);
		return 1;
	}

	if (listen(sock, backlog) < 0) {
		logerr("failed to listen on %s: %s\n", lsnr->ip, strerror(errno));
		close(sock);
		return 1;
	}

	logout("listening on UNIX socket %s\n", lsnr->ip);

	return 0;
}

/**
 * Open up sockets associated with listener.  Returns 0 when opening up
 * the listener succeeded, 1 otherwise.
 */
char
bindlisten(listener *lsnr, unsigned int backlog)
{
	switch (lsnr->ctype) {
		case CON_TCP:
		case CON_UDP:
			return bindlistenip(lsnr, backlog);
		case CON_UNIX:
			return bindlistenunix(lsnr, backlog);
		default:
			logerr("unsupported listener type");
			return 1;
	}
}

static void
close_socks(listener *lsnr)
{
	int i;
	for (i = 0; lsnr->socks[i] != -1; i++)
		close(lsnr->socks[i]);
	logout("closed listener for %s:%u\n", lsnr->ip, lsnr->port);
}

static void
destroy_usock(listener *lsnr)
{
	close(lsnr->socks[0]);
	unlink(lsnr->ip);
	logout("closed listener for %s\n", lsnr->ip);
}

void
shutdownclose(listener *lsnr)
{
	switch (lsnr->ctype) {
		case CON_TCP:
		case CON_UDP:
			close_socks(lsnr);
			break;
		case CON_UNIX:
			destroy_usock(lsnr);
			break;
		default:
			logerr("unsupported listener type");
			break;
	}
}
