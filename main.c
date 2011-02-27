/*
 * Copy me if you can.
 * by 20h
 */

#include <unistd.h>
#include <dirent.h>
#include <memory.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>

#include "ind.h"
#include "handlr.h"
#include "arg.h"

enum {
	NOLOG	= 0,
	FILES	= 1,
	DIRS	= 2,
	HTTP	= 4,
	ERRORS	= 8,
};

int glfd = -1;
int loglvl = 15;
int running = 1;
char *logfile = nil;

char *argv0;
char *stdbase = "/var/gopher";
char *stdport = "70";
char *indexf = "/index.gph";
char *err = "0Sorry, but the requested token could not be found\tErr"
	    "\tlocalhost\t70\r\n.\r\n\r\n";
char *htredir = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"\n"
		"	\"DTD/xhtml-transitional.dtd\">\n"
		"<html xmlns=\"http://www.w3.org/1999/xhtml\" lang=\"en\">\n"
		"  <head>\n"
		"    <title>gopher redirect</title>\n"
		"\n"
		"    <meta http-equiv=\"Refresh\" content=\"1;url=%s\" />\n"
		"  </head>\n"
		"  <body>\n"
		"    This page is for redirecting you to: <a href=\"%s\">%s</a>.\n"
		"  </body>\n"
		"</html>\n";

int
dropprivileges(struct group *gr, struct passwd *pw)
{
	if(gr != nil)
		if(setgroups(1, &gr->gr_gid) != 0 || setgid(gr->gr_gid) != 0)
			return -1;
	if(pw != nil) {
		if(gr == nil) {
			if(setgroups(1, &pw->pw_gid) != 0 ||
			    setgid(pw->pw_gid) != 0)
				return -1;
		}
		if(setuid(pw->pw_uid) != 0)
			return -1;
	}

	return 0;
}

char *
securepath(char *p, int len)
{
	int i;

	if(len < 2)
		return p;

	for(i = 1; i < strlen(p); i++) {
		if(p[i - 1] == '.' && p[i] == '.') {
			if(p[i - 2] == '/')
				p[i] = '/';
			if(p[i + 1] == '/')
				p[i] = '/';
			if(len == 2)
				p[i] = '/';
		}
	}

	return p;
}

void
logentry(char *host, char *port, char *qry, char *status)
{
	time_t tim;
	struct tm *ptr;
	char timstr[128], *ahost;

        if(glfd >= 0) {
		tim = time(0);
		ptr = localtime(&tim);

		ahost = reverselookup(host);
		strftime(timstr, sizeof(timstr), "%a %b %d %H:%M:%S %Z %Y",
					ptr);

		tprintf(glfd, "[%s|%s:%s] %s (%s)\n",
			timstr, ahost, port, qry, status);
		free(ahost);
        }

	return;
}

void
handlerequest(int sock, char *base, char *ohost, char *port, char *clienth,
			char *clientp)
{
	struct stat dir;
	char recvc[1024], recvb[1024], path[1024], *args, *sear, *c;
	int len, fd;
	filetype *type;

	memset(&dir, 0, sizeof(dir));
	memset(recvb, 0, sizeof(recvb));
	memset(recvc, 0, sizeof(recvc));

	len = recv(sock, recvb, sizeof(recvb), 0);
	if(len > 1) {
		if(recvb[len - 2] == '\r')
			recvb[len - 2] = '\0';
		if(recvb[len - 1] == '\n')
			recvb[len - 1] = '\0';
	}
	strcpy(recvc, recvb);

	if(!strncmp(recvb, "URL:", 4)) {
		len = snprintf(path, sizeof(path), htredir,
				recvb + 4, recvb + 4, recvb + 4);
		if(len > sizeof(path))
			len = sizeof(path);
		send(sock, path, len, 0);
		if(loglvl & HTTP)
			logentry(clienth, clientp, recvc, "HTTP redirect");
		return;
	}

	sear = strchr(recvb, '\t');
	if(sear != nil)
		*sear++ = '\0';
	args = strchr(recvb, '?');
	if(args != nil)
		*args++ = '\0';
	else
		args = ohost;

	securepath(recvb, len - 2);
	snprintf(path, sizeof(path), "%s%s", base, recvb);
	if(stat(path, &dir) != -1 && S_ISDIR(dir.st_mode))
		strncat(path, indexf, sizeof(path) - strlen(path));

	fd = open(path, O_RDONLY);
	if(fd >= 0) {
		close(fd);
		if(loglvl & FILES)
			logentry(clienth, clientp, recvc, "serving");

		c = strrchr(path, '/');
		if(c == nil)
			c = path;
		type = gettype(c);
		type->f(sock, path, port, base, args, sear);
	} else {
		if(S_ISDIR(dir.st_mode)) {
			handledir(sock, path, port, base, args, sear);
			if(loglvl & DIRS)
				logentry(clienth, clientp, recvc,
							"dir listing");
			return;
		}

		send(sock, err, strlen(err), 0);
		if(loglvl & ERRORS)
			logentry(clienth, clientp, recvc, "not found");
		close(sock);
	}

	return;
}

void
sighandler(int sig)
{
	switch(sig) {
	case SIGCHLD:
		while(waitpid(-1, NULL, WNOHANG) > 0);
		break;
	case SIGHUP:
	case SIGINT:
	case SIGQUIT:
	case SIGABRT:
	case SIGTERM:
		if(logfile != nil)
			stoplogging(glfd);
		running = 0;
		break;
	default:
		break;
	}
}

void
initsignals(void)
{
	signal(SIGCHLD, sighandler);
	signal(SIGHUP, sighandler);
	signal(SIGINT, sighandler);
	signal(SIGQUIT, sighandler);
	signal(SIGABRT, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGKILL, sighandler);
}

void
usage(void)
{
	tprintf(2, "usage: %s [-d] [-l logfile] [-v loglvl] [-b base]"
		   " [-p port] [-o sport] [-u user] [-g group] [-h host]"
		   " [-i IP]\n",
		   argv0);

	exit(1);
}

int
main(int argc, char *argv[])
{
	struct addrinfo hints, *ai;
	struct sockaddr_storage clt;
	socklen_t cltlen;
	int sock, list, opt, dofork;
	char *port, *base, clienth[NI_MAXHOST], clientp[NI_MAXSERV];
	char *user, *group, *bindip, *ohost, *sport;
	struct passwd *us;
	struct group *gr;

	base = stdbase;
	port = stdport;
	dofork = 1;
	user = nil;
	group = nil;
	us = nil;
	gr = nil;
	bindip = nil;
	ohost = nil;
	sport = port;

	ARGBEGIN {
	case 'b':
		base = EARGF(usage());
		break;
	case 'p':
		port = EARGF(usage());
		break;
	case 'l':
		logfile = EARGF(usage());
		break;
	case 'd':
		dofork = 0;
		break;
	case 'v':
		loglvl = atoi(EARGF(usage()));
		break;
	case 'u':
		user = EARGF(usage());
		break;
	case 'g':
		group = EARGF(usage());
		break;
	case 'i':
		bindip = EARGF(usage());
		break;
	case 'h':
		ohost = EARGF(usage());
		break;
	case 'o':
		sport = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND;

	if(group != nil) {
		if((gr = getgrnam(group)) == nil) {
			perror("no such group");
			return 1;
		}
	}

	if(user != nil) {
		if((us = getpwnam(user)) == nil) {
			perror("no such user");
			return 1;
		}
	}

	if(dofork && fork() != 0)
		return 0;

	if(logfile != nil) {
		glfd = initlogging(logfile);
		if(glfd < 0) {
			perror("initlogging");
			return 1;
		}
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_flags |= AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_family = AF_INET;
	if(getaddrinfo(bindip, port, &hints, &ai)) {
		perror("getaddrinfo");
		return 1;
	}
	if(ai == nil) {
		perror("getaddrinfo");
		return 1;
	}

	list = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if(list < 0) {
		perror("socket");
		return 1;
	}

	opt = 1;
	if(setsockopt(list, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
		perror("setsockopt");
		return 1;
	}

	if(bind(list, ai->ai_addr, ai->ai_addrlen)) {
		perror("bind");
		return 1;
	}

	if(listen(list, 255)) {
		perror("listen");
		return 1;
	}

	freeaddrinfo(ai);

	if(dropprivileges(gr, us) < 0) {
		perror("cannot drop privileges");
		return 1;
	}

	initsignals();

	cltlen = sizeof(clt);
	while(running) {
		sock = accept(list, (struct sockaddr *)&clt, &cltlen);
		if(sock < 0) {
			switch(errno) {
			case ECONNABORTED:
			case EINTR:
				continue;
			default:
				perror("accept");
				close(list);
				return 1;
			}
		}

		getnameinfo((struct sockaddr *)&clt, cltlen, clienth,
				sizeof(clienth), clientp, sizeof(clientp),
				NI_NUMERICHOST);

		switch(fork()) {
		case -1:
			perror("fork");
			close(sock);
			break;
		case 0:
			handlerequest(sock, base, ohost, sport, clienth,
						clientp);
			return 0;
		default:
			wait(&opt);
			close(sock);
			break;
		}
	}

	close(list);
	if(logfile != nil)
		stoplogging(glfd);
	return 0;
}

