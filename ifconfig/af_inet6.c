/*
 * Copyright (c) 2009-2017, 2020 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE || TARGET_OS_MACCATALYST || TARGET_OS_WATCH || TARGET_OS_TV
#include "ios_error.h"
#endif

#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ifaddrs.h>

#include <arpa/inet.h>

#include <netinet/in.h>
#include <net/if_var.h>		/* for struct ifaddr */
#if TARGET_OS_MACCATALYST
#include <netinet/in_var.h>
#include <netinet6/nd6.h>
#endif
#include <arpa/inet.h>
#include <netdb.h>
#include "netinet6/in6_var.h"
#include "netinet6/nd6.h"

#include "ifconfig.h"

extern	char *f_inet6, *f_addr;

#define ND6BITS "\020\001PERFORMNUD\002ACCEPT_RTADV\003PREFER_SOURCE" \
	"\004IFDISABLED\005DONT_SET_IFROUTE\006PROXY_PREFIXES" \
	"\007IGNORE_NA\010INSECURE\011REPLICATED\012DAD"

static	struct in6_ifreq in6_ridreq;
static	struct in6_aliasreq in6_addreq = 
  { { 0 }, 
    { 0 }, 
    { 0 }, 
    { 0 }, 
    0, 
    { 0, 0, ND6_INFINITE_LIFETIME, ND6_INFINITE_LIFETIME } };

#if !(TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
static	int ip6lifetime;
#else
static __thread int ip6lifetime = 0;
#endif

static	void in6_fillscopeid(struct sockaddr_in6 *sin6);
static	int prefix(void *, int);
static	char *sec2str(time_t);
static	int explicit_prefix = 0;

static	char addr_buf[MAXHOSTNAMELEN *2 + 1];	/*for getnameinfo()*/

static void
setifprefixlen(const char *addr, int dummy __unused, int s,
    const struct afswtch *afp)
{
        if (afp->af_getprefix != NULL)
                afp->af_getprefix(addr, MASK);
	explicit_prefix = 1;
}

static void
setnd6flags(const char *dummyaddr __unused, int d, int s,
    const struct afswtch *afp)
{
	struct in6_ndireq nd;
	int error;

	memset(&nd, 0, sizeof(nd));
	strlcpy(nd.ifname, ifr.ifr_name, sizeof(nd.ifname));
	error = ioctl(s, SIOCGIFINFO_IN6, &nd);
	if (error) {
		warn("ioctl(SIOCGIFINFO_IN6)");
		return;
	}
	if (d < 0)
		nd.ndi.flags &= ~(-d);
	else
		nd.ndi.flags |= d;
	error = ioctl(s, SIOCSIFINFO_FLAGS, (caddr_t)&nd);
	if (error)
		warn("ioctl(SIOCSIFINFO_FLAGS)");
}

static void
setip6flags(const char *dummyaddr __unused, int flag, int dummysoc __unused,
    const struct afswtch *afp)
{
	if (afp->af_af != AF_INET6)
		err(1, "address flags can be set only for inet6 addresses");

	if (flag < 0)
		in6_addreq.ifra_flags &= ~(-flag);
	else
		in6_addreq.ifra_flags |= flag;
}

static void
setip6lifetime(const char *cmd, const char *val, int s, 
    const struct afswtch *afp)
{
	time_t newval, t;
	char *ep;

	t = time(NULL);
	newval = (time_t)strtoul(val, &ep, 0);
	if (val == ep)
		errx(1, "invalid %s", cmd);
	if (afp->af_af != AF_INET6)
		errx(1, "%s not allowed for the AF", cmd);
	if (strcmp(cmd, "vltime") == 0) {
		in6_addreq.ifra_lifetime.ia6t_expire = t + newval;
		in6_addreq.ifra_lifetime.ia6t_vltime = newval;
	} else if (strcmp(cmd, "pltime") == 0) {
		in6_addreq.ifra_lifetime.ia6t_preferred = t + newval;
		in6_addreq.ifra_lifetime.ia6t_pltime = newval;
	}
}

static void
setip6pltime(const char *seconds, int dummy __unused, int s, 
    const struct afswtch *afp)
{
	setip6lifetime("pltime", seconds, s, afp);
}

static void
setip6vltime(const char *seconds, int dummy __unused, int s, 
    const struct afswtch *afp)
{
	setip6lifetime("vltime", seconds, s, afp);
}

static void
setip6eui64(const char *cmd, int dummy __unused, int s,
    const struct afswtch *afp)
{
	struct ifaddrs *ifap, *ifa;
	const struct sockaddr_in6 *sin6 = NULL;
	const struct in6_addr *lladdr = NULL;
	struct in6_addr *in6;

	if (afp->af_af != AF_INET6)
		errx(EXIT_FAILURE, "%s not allowed for the AF", cmd);
 	in6 = (struct in6_addr *)&in6_addreq.ifra_addr.sin6_addr;
	if (memcmp(&in6addr_any.s6_addr[8], &in6->s6_addr[8], 8) != 0)
		errx(EXIT_FAILURE, "interface index is already filled");
	if (getifaddrs(&ifap) != 0)
		err(EXIT_FAILURE, "getifaddrs");
	for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr->sa_family == AF_INET6 &&
		    strcmp(ifa->ifa_name, name) == 0) {
			sin6 = (const struct sockaddr_in6 *)ifa->ifa_addr;
			if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
				lladdr = &sin6->sin6_addr;
				break;
			}
		}
	}
	if (!lladdr)
		errx(EXIT_FAILURE, "could not determine link local address"); 

 	memcpy(&in6->s6_addr[8], &lladdr->s6_addr[8], 8);

	freeifaddrs(ifap);
}

static void
in6_fillscopeid(struct sockaddr_in6 *sin6)
{
#if defined(__KAME__) && defined(KAME_SCOPEID)
	if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
		sin6->sin6_scope_id =
			ntohs(*(u_int16_t *)&sin6->sin6_addr.s6_addr[2]);
		sin6->sin6_addr.s6_addr[2] = sin6->sin6_addr.s6_addr[3] = 0;
	}
#endif
}

static void
in6_status(int s __unused, const struct ifaddrs *ifa)
{
	struct sockaddr_in6 *sin, null_sin;
	struct in6_ifreq ifr6;
	int s6;
	u_int32_t flags6;
	struct in6_addrlifetime lifetime;
	time_t t = time(NULL);
	int error, n_flags;
	u_int32_t scopeid;

	memset(&null_sin, 0, sizeof(null_sin));

	sin = (struct sockaddr_in6 *)ifa->ifa_addr;
	if (sin == NULL)
		return;

	strlcpy(ifr6.ifr_name, ifr.ifr_name, sizeof(ifr.ifr_name));
	if ((s6 = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
		warn("socket(AF_INET6,SOCK_DGRAM)");
		return;
	}
	ifr6.ifr_addr = *sin;
	if (ioctl(s6, SIOCGIFAFLAG_IN6, &ifr6) < 0) {
		warn("ioctl(SIOCGIFAFLAG_IN6)");
		close(s6);
		return;
	}
	flags6 = ifr6.ifr_ifru.ifru_flags6;
	memset(&lifetime, 0, sizeof(lifetime));
	ifr6.ifr_addr = *sin;
	if (ioctl(s6, SIOCGIFALIFETIME_IN6, &ifr6) < 0) {
		warn("ioctl(SIOCGIFALIFETIME_IN6)");
		close(s6);
		return;
	}
	lifetime = ifr6.ifr_ifru.ifru_lifetime;
	close(s6);

	/* XXX: embedded link local addr check */
	if (IN6_IS_ADDR_LINKLOCAL(&sin->sin6_addr) &&
	    *(u_short *)&sin->sin6_addr.s6_addr[2] != 0) {
		u_short index;

		index = *(u_short *)&sin->sin6_addr.s6_addr[2];
		*(u_short *)&sin->sin6_addr.s6_addr[2] = 0;
		if (sin->sin6_scope_id == 0)
			sin->sin6_scope_id = ntohs(index);
	}
	scopeid = sin->sin6_scope_id;

	if (f_addr != NULL && strcmp(f_addr, "fqdn") == 0)
		n_flags = 0;
	else if (f_addr != NULL && strcmp(f_addr, "host") == 0)
		n_flags = NI_NOFQDN;
	else
		n_flags = NI_NUMERICHOST;
	error = getnameinfo((struct sockaddr *)sin, sin->sin6_len, addr_buf,
			    sizeof(addr_buf), NULL, 0, n_flags);
	if (error != 0)
		inet_ntop(AF_INET6, &sin->sin6_addr, addr_buf,
			  sizeof(addr_buf));
	printf("\tinet6 %s", addr_buf);

	if (ifa->ifa_flags & IFF_POINTOPOINT) {
		sin = (struct sockaddr_in6 *)ifa->ifa_dstaddr;
		/*
		 * some of the interfaces do not have valid destination
		 * address.
		 */
		if (sin != NULL && sin->sin6_family == AF_INET6) {
			int error;

			/* XXX: embedded link local addr check */
			if (IN6_IS_ADDR_LINKLOCAL(&sin->sin6_addr) &&
			    *(u_short *)&sin->sin6_addr.s6_addr[2] != 0) {
				u_short index;

				index = *(u_short *)&sin->sin6_addr.s6_addr[2];
				*(u_short *)&sin->sin6_addr.s6_addr[2] = 0;
				if (sin->sin6_scope_id == 0)
					sin->sin6_scope_id = ntohs(index);
			}

			error = getnameinfo((struct sockaddr *)sin,
					    sin->sin6_len, addr_buf,
					    sizeof(addr_buf), NULL, 0,
					    NI_NUMERICHOST);
			if (error != 0)
				inet_ntop(AF_INET6, &sin->sin6_addr, addr_buf,
					  sizeof(addr_buf));
			printf(" --> %s", addr_buf);
		}
	}

	sin = (struct sockaddr_in6 *)ifa->ifa_netmask;
	if (sin == NULL)
		sin = &null_sin;
	if (f_inet6 != NULL && strcmp(f_inet6, "cidr") == 0)
		printf("/%d ", prefix(&sin->sin6_addr,
			sizeof(struct in6_addr)));
	else
		printf(" prefixlen %d ", prefix(&sin->sin6_addr,
			sizeof(struct in6_addr)));

	if ((flags6 & IN6_IFF_ANYCAST) != 0)
		printf("anycast ");
	if ((flags6 & IN6_IFF_TENTATIVE) != 0)
		printf("tentative ");
	if ((flags6 & IN6_IFF_OPTIMISTIC) != 0)
		printf("optimistic ");
	if ((flags6 & IN6_IFF_DUPLICATED) != 0)
		printf("duplicated ");
	if ((flags6 & IN6_IFF_DETACHED) != 0)
		printf("detached ");
	if ((flags6 & IN6_IFF_DEPRECATED) != 0)
		printf("deprecated ");
	if ((flags6 & IN6_IFF_AUTOCONF) != 0)
		printf("autoconf ");
	if ((flags6 & IN6_IFF_TEMPORARY) != 0)
		printf("temporary ");
	if ((flags6 & IN6_IFF_DYNAMIC) != 0)
		printf("dynamic ");
	if ((flags6 & IN6_IFF_SECURED) != 0)
		printf("secured ");
	if ((flags6 & IN6_IFF_CLAT46) != 0)
		printf("clat46 ");

        if (scopeid)
		printf("scopeid 0x%x ", scopeid);

	if (ip6lifetime && (lifetime.ia6t_preferred || lifetime.ia6t_expire)) {
		printf("pltime ");
		if (lifetime.ia6t_preferred) {
			printf("%s ", lifetime.ia6t_preferred < t
				? "0" : sec2str(lifetime.ia6t_preferred - t));
		} else
			printf("infty ");

		printf("vltime ");
		if (lifetime.ia6t_expire) {
			printf("%s ", lifetime.ia6t_expire < t
				? "0" : sec2str(lifetime.ia6t_expire - t));
		} else
			printf("infty ");
	}

	putchar('\n');
}

#define	SIN6(x) ((struct sockaddr_in6 *) &(x))
static struct	sockaddr_in6 *sin6tab[] = {
	SIN6(in6_ridreq.ifr_addr), SIN6(in6_addreq.ifra_addr),
	SIN6(in6_addreq.ifra_prefixmask), SIN6(in6_addreq.ifra_dstaddr)
};

static void
in6_getprefix(const char *plen, int which)
{
	struct sockaddr_in6 *sin = sin6tab[which];
	u_char *cp;
	int len = atoi(plen);

	if ((len < 0) || (len > 128))
		errx(1, "%s: bad value", plen);
	sin->sin6_len = sizeof(*sin);
	if (which != MASK)
		sin->sin6_family = AF_INET6;
	if ((len == 0) || (len == 128)) {
		memset(&sin->sin6_addr, 0xff, sizeof(struct in6_addr));
		return;
	}
	memset((void *)&sin->sin6_addr, 0x00, sizeof(sin->sin6_addr));
	for (cp = (u_char *)&sin->sin6_addr; len > 7; len -= 8)
		*cp++ = 0xff;
	*cp = 0xff << (8 - len);
}

static void
in6_getaddr(const char *s, int which)
{
	struct sockaddr_in6 *sin = sin6tab[which];
	struct addrinfo hints, *res;
	int error = -1;

	newaddr &= 1;

	sin->sin6_len = sizeof(*sin);
	if (which != MASK)
		sin->sin6_family = AF_INET6;

	if (which == ADDR) {
		char *p = NULL;
		if((p = strrchr(s, '/')) != NULL) {
			*p = '\0';
			in6_getprefix(p + 1, MASK);
			explicit_prefix = 1;
		}
	}

	if (sin->sin6_family == AF_INET6) {
		bzero(&hints, sizeof(struct addrinfo));
		hints.ai_family = AF_INET6;
		error = getaddrinfo(s, NULL, &hints, &res);
	}
	if (error != 0) {
		if (inet_pton(AF_INET6, s, &sin->sin6_addr) != 1)
			errx(1, "%s: bad value", s);
	} else
		bcopy(res->ai_addr, sin, res->ai_addrlen);
}

static int
prefix(void *val, int size)
{
        u_char *name = (u_char *)val;
        int byte, bit, plen = 0;

        for (byte = 0; byte < size; byte++, plen += 8)
                if (name[byte] != 0xff)
                        break;
	if (byte == size)
		return (plen);
	for (bit = 7; bit != 0; bit--, plen++)
                if (!(name[byte] & (1 << bit)))
                        break;
        for (; bit != 0; bit--)
                if (name[byte] & (1 << bit))
                        return(0);
        byte++;
        for (; byte < size; byte++)
                if (name[byte])
                        return(0);
        return (plen);
}

static char *
sec2str(time_t total)
{
	static char result[256];
	int days, hours, mins, secs;
	int first = 1;
	char *p = result;

	if (0) {
		days = total / 3600 / 24;
		hours = (total / 3600) % 24;
		mins = (total / 60) % 60;
		secs = total % 60;

		if (days) {
			first = 0;
			p += snprintf(p, sizeof(result) - (p - result), "%dd", days);
		}
		if (!first || hours) {
			first = 0;
			p += snprintf(p, sizeof(result) - (p - result), "%dh", hours);
		}
		if (!first || mins) {
			first = 0;
			p += snprintf(p, sizeof(result) - (p - result), "%dm", mins);
		}
		snprintf(p, sizeof(result) - (p - result), "%ds", secs);
	} else
		snprintf(result, sizeof(result), "%lu", (unsigned long)total);

	return(result);
}

static void
in6_postproc(int s, const struct afswtch *afp)
{
	if (explicit_prefix == 0) {
		/* Aggregatable address architecture defines all prefixes
		   are 64. So, it is convenient to set prefixlen to 64 if
		   it is not specified. */
		setifprefixlen("64", 0, s, afp);
		/* in6_getprefix("64", MASK) if MASK is available here... */
	}
}

static void
in6_status_tunnel(int s)
{
	char src[NI_MAXHOST];
	char dst[NI_MAXHOST];
	struct in6_ifreq in6_ifr;
	const struct sockaddr *sa = (const struct sockaddr *) &in6_ifr.ifr_addr;

	memset(&in6_ifr, 0, sizeof(in6_ifr));
	strlcpy(in6_ifr.ifr_name, name, sizeof(in6_ifr.ifr_name));

	if (ioctl(s, SIOCGIFPSRCADDR_IN6, (caddr_t)&in6_ifr) < 0)
		return;
	if (sa->sa_family != AF_INET6)
		return;
	in6_fillscopeid(&in6_ifr.ifr_addr);
	if (getnameinfo(sa, sa->sa_len, src, sizeof(src), 0, 0,
	    NI_NUMERICHOST) != 0)
		src[0] = '\0';

	if (ioctl(s, SIOCGIFPDSTADDR_IN6, (caddr_t)&in6_ifr) < 0)
		return;
	if (sa->sa_family != AF_INET6)
		return;
	in6_fillscopeid(&in6_ifr.ifr_addr);
	if (getnameinfo(sa, sa->sa_len, dst, sizeof(dst), 0, 0,
	    NI_NUMERICHOST) != 0)
		dst[0] = '\0';

	printf("\ttunnel inet6 %s --> %s\n", src, dst);
}

static void
nd6_status(int s)
{
	struct in6_ndireq nd;
	int s6;
	int error;

	memset(&nd, 0, sizeof(nd));
	strlcpy(nd.ifname, ifr.ifr_name, sizeof(nd.ifname));
	if ((s6 = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
		if (errno != EPROTONOSUPPORT)
			warn("socket(AF_INET6, SOCK_DGRAM)");
		return;
	}
	error = ioctl(s6, SIOCGIFINFO_IN6, &nd);
	if (error) {
		if (errno != EPFNOSUPPORT && errno != EINVAL)
			warn("ioctl(SIOCGIFINFO_IN6)");
		close(s6);
		return;
	}
	close(s6);
	if (nd.ndi.flags == 0)
		return;
	printb("\tnd6 options", (unsigned int)nd.ndi.flags, ND6BITS);
	putchar('\n');
}

static void
in6_set_tunnel(int s, struct addrinfo *srcres, struct addrinfo *dstres)
{
	struct in6_aliasreq in6_addreq; 

	memset(&in6_addreq, 0, sizeof(in6_addreq));
	strlcpy(in6_addreq.ifra_name, name, sizeof(in6_addreq.ifra_name));
	memcpy(&in6_addreq.ifra_addr, srcres->ai_addr, srcres->ai_addr->sa_len);
	memcpy(&in6_addreq.ifra_dstaddr, dstres->ai_addr,
	    dstres->ai_addr->sa_len);

	if (ioctl(s, SIOCSIFPHYADDR_IN6, &in6_addreq) < 0)
		warn("SIOCSIFPHYADDR_IN6");
}

#ifndef IPV6_ROUTER_MODE_EXCLUSIVE
#define IPV6_ROUTER_MODE_DISABLED       0
#define IPV6_ROUTER_MODE_EXCLUSIVE      1
#define IPV6_ROUTER_MODE_HYBRID         2
#endif /* IPV6_ROUTER_MODE_EXCLUSIVE */

#if !(TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
static void
in6_set_router(int s, int enable)
{
	struct ifreq ifr;

	bzero(&ifr, sizeof (ifr));
	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
	ifr.ifr_intval = (enable == 0)
		? IPV6_ROUTER_MODE_DISABLED
		: IPV6_ROUTER_MODE_EXCLUSIVE;

	if (ioctl(s, SIOCSETROUTERMODE_IN6, &ifr) < 0)
		warn("SIOCSETROUTERMODE_IN6");
}
#endif

static int
routermode_from_string(char * str, int *mode_p)
{
	int	success = 1;

	if (strcasecmp(str, "exclusive") == 0 ||
	    strcasecmp(str, "enabled") == 0) {
		*mode_p = IPV6_ROUTER_MODE_EXCLUSIVE;
	} else if (strcasecmp(str, "hybrid") == 0) {
		*mode_p = IPV6_ROUTER_MODE_HYBRID;
	} else if (strcasecmp(str, "disabled") == 0) {
		*mode_p = IPV6_ROUTER_MODE_DISABLED;
	} else {
		success = 0;
	}
	return (success);
}

static const char *
routermode_string(int mode)
{
	const char *	str;

	switch (mode) {
	case IPV6_ROUTER_MODE_EXCLUSIVE:
		str = "enabled";
		break;
	case IPV6_ROUTER_MODE_HYBRID:
		str = "hybrid";
		break;
	case IPV6_ROUTER_MODE_DISABLED:
		str = "disabled";
		break;
	default:
		str = "<unknown>";
		break;
	}
	return str;
}

#if !(TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
static int
in6_routermode(int s, int argc, char *const*argv)
{
	struct in6_ifreq	ifr;
	int 			ret;

	bzero(&ifr, sizeof (ifr));
	strlcpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));
	if (argc == 0) {
		ret = 0;
#ifndef SIOCGETROUTERMODE_IN6
#define SIOCGETROUTERMODE_IN6   _IOWR('i', 137, struct in6_ifreq)
#endif /* SIOCGETROUTERMODE_IN6 */
		if (ioctl(s, SIOCGETROUTERMODE_IN6, &ifr) < 0) {
			if (argv != NULL) {
				warn("SIOCGETROUTERMODE_IN6");
			}
		} else {
			/* argv is NULL if we're called from status() */
			printf("%s%s\n",
			       (argv == NULL) ? "\troutermode6: " : "",
			       routermode_string(ifr.ifr_intval));
		}
		ret = 0;
	} else {
		int mode;

		if (routermode_from_string(argv[0], &mode) == 0) {
			errx(EXIT_FAILURE,
			     "mode '%s' invalid, must be one of "
			     "disabled, exclusive, or hybrid",
			     argv[0]);
		}
		ifr.ifr_intval = mode;
		if (ioctl(s, SIOCSETROUTERMODE_IN6, &ifr) < 0) {
			warn("SIOCSETROUTERMODE_IN6");
		}
		ret = 1;
	}
	return ret;
}
#endif

static struct cmd inet6_cmds[] = {
	DEF_CMD_ARG("prefixlen",			setifprefixlen),
	DEF_CMD("anycast",	IN6_IFF_ANYCAST,	setip6flags),
	DEF_CMD("tentative",	IN6_IFF_TENTATIVE,	setip6flags),
	DEF_CMD("-tentative",	-IN6_IFF_TENTATIVE,	setip6flags),
	/* RFC 4429, section 3.1, says:
	 * "Optimistic DAD SHOULD NOT be used for manually entered
	 * addresses."
	 * it's not a MUST...
	 */
	DEF_CMD("optimistic",	IN6_IFF_OPTIMISTIC,	setip6flags),
	DEF_CMD("-optimistic",	-IN6_IFF_OPTIMISTIC,	setip6flags),
	DEF_CMD("deprecated",	IN6_IFF_DEPRECATED,	setip6flags),
	DEF_CMD("-deprecated", -IN6_IFF_DEPRECATED,	setip6flags),
	DEF_CMD("autoconf",	IN6_IFF_AUTOCONF,	setip6flags),
	DEF_CMD("-autoconf",	-IN6_IFF_AUTOCONF,	setip6flags),
	DEF_CMD("nud",		ND6_IFF_PERFORMNUD,	setnd6flags),
	DEF_CMD("-nud",		-ND6_IFF_PERFORMNUD,	setnd6flags),
#if !(TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
	DEF_CMD("ifdisabled",   ND6_IFF_IFDISABLED,	setnd6flags),
	DEF_CMD("-ifdisabled",  -ND6_IFF_IFDISABLED,	setnd6flags),
#endif
	DEF_CMD("replicated",	ND6_IFF_REPLICATED,	setnd6flags),
	DEF_CMD("-replicated",	-ND6_IFF_REPLICATED,	setnd6flags),
	DEF_CMD("proxy_prefixes", ND6_IFF_PROXY_PREFIXES,	setnd6flags),
	DEF_CMD("-proxy_prefixes", -ND6_IFF_PROXY_PREFIXES,	setnd6flags),
#if !(TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
	DEF_CMD("insecure",	ND6_IFF_INSECURE,	setnd6flags),
	DEF_CMD("-insecure",	-ND6_IFF_INSECURE,	setnd6flags),
#endif
	DEF_CMD_ARG("pltime",        			setip6pltime),
	DEF_CMD_ARG("vltime",        			setip6vltime),
	DEF_CMD("eui64",	0,			setip6eui64),
	DEF_CMD("secured",	IN6_IFF_SECURED,	setip6flags),
	DEF_CMD("-secured",	-IN6_IFF_SECURED,	setip6flags),
	DEF_CMD("dad",		ND6_IFF_DAD,		setnd6flags),
	DEF_CMD("-dad",		-ND6_IFF_DAD,		setnd6flags),
};

static struct afswtch af_inet6 = {
	.af_name	= "inet6",
	.af_af		= AF_INET6,
	.af_status	= in6_status,
	.af_getaddr	= in6_getaddr,
	.af_getprefix	= in6_getprefix,
	.af_other_status = nd6_status,
	.af_postproc	= in6_postproc,
	.af_status_tunnel = in6_status_tunnel,
	.af_settunnel	= in6_set_tunnel,
#if !(TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
	.af_setrouter	= in6_set_router,
	.af_routermode	= in6_routermode,
#endif
	.af_difaddr	= SIOCDIFADDR_IN6,
	.af_aifaddr	= SIOCAIFADDR_IN6,
	.af_ridreq	= &in6_ridreq,
	.af_addreq	= &in6_addreq,
};

static void
in6_Lopt_cb(const char *optarg __unused)
{
	ip6lifetime++;	/* print IPv6 address lifetime */
}
static struct ifconfig_option in6_Lopt = { "L", "[-L]", in6_Lopt_cb };

static __constructor void
inet6_ctor(void)
{
#define	N(a)	(sizeof(a) / sizeof(a[0]))
	int i;

	for (i = 0; i < N(inet6_cmds);  i++)
		cmd_register(&inet6_cmds[i]);
	af_register(&af_inet6);
	opt_register(&in6_Lopt);
#undef N
}
