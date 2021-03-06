/*
 * Copyright (C) 1994-2018 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * PBS Pro is free software. You can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * PBS Pro is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * For a copy of the commercial license terms and conditions,
 * go to: (http://www.pbspro.com/UserArea/agreement.html)
 * or contact the Altair Legal Department.
 *
 * Altair’s dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of PBS Pro and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair’s trademarks, including but not limited to "PBS™",
 * "PBS Professional®", and "PBS Pro™" and Altair’s logos is subject to Altair's
 * trademark licensing policies.
 *
 */
/**
 * @file	pbs_connect.c
 * @brief
 *	Open a connection with the pbs server.  At this point several
 *	things are stubbed out, and other things are hard-wired.
 *
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <pwd.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#ifndef WIN32
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#include <pbs_ifl.h>
#include "libpbs.h"
#include "net_connect.h"
#include "dis.h"
#include "libsec.h"
#include "pbs_ecl.h"
#include "pbs_internal.h"


extern struct connect_handle connection[NCONNECTS];

#define ERR_BUF_SIZE 4096

/**
 * @brief
 *	-returns the default server name.
 *
 * @return	string
 * @retval	dflt srvr name	success
 * @retval	NULL		error
 *
 */
char *
__pbs_default()
{
	char dflt_server[PBS_MAXSERVERNAME+1];
	struct pbs_client_thread_context *p;

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return NULL;

	p =  pbs_client_thread_get_context_data();

	if (pbs_loadconf(0) == 0)
		return NULL;

	if (p->th_pbs_defserver[0] == '\0') {
		/* The check for PBS_DEFAULT is done in pbs_loadconf() */
		if (pbs_conf.pbs_primary && pbs_conf.pbs_secondary) {
			strncpy(dflt_server, pbs_conf.pbs_primary, PBS_MAXSERVERNAME);
		} else if (pbs_conf.pbs_server_host_name) {
			strncpy(dflt_server, pbs_conf.pbs_server_host_name, PBS_MAXSERVERNAME);
		} else if (pbs_conf.pbs_server_name) {
			strncpy(dflt_server, pbs_conf.pbs_server_name, PBS_MAXSERVERNAME);
		} else {
			dflt_server[0] = '\0';
		}
		strcpy(p->th_pbs_defserver, dflt_server);
	}
	return (p->th_pbs_defserver);
}

/**
 * @brief
 *	-returns the server name.
 *
 * @param[in] server - server name
 * @param[out] server_name - server name
 * @param[in] port - port number
 *
 * @return	string
 * @retval	servr name	success
 *
 */
static char *
PBS_get_server(char *server, char *server_name,
	unsigned int *port)
{
	int   i;
	char *pc;
	unsigned int dflt_port = 0;
	char *p;

	for (i=0;i<PBS_MAXSERVERNAME+1;i++)
		server_name[i] = '\0';

	if (dflt_port == 0)
		dflt_port = pbs_conf.batch_service_port;

	/* first, get the "net.address[:port]" into 'server_name' */

	if ((server == NULL) || (*server == '\0')) {
		if ((p=pbs_default()) == NULL)
			return NULL;
		strcpy(server_name, p);
	} else {
		strncpy(server_name, server, PBS_MAXSERVERNAME);
	}

	/* now parse out the parts from 'server_name' */

	if ((pc = strchr(server_name, (int)':')) != NULL) {
		/* got a port number */
		*pc++ = '\0';
		*port = atoi(pc);
	} else {
		*port = dflt_port;
	}

	return server_name;
}

/*
 * @brief
 *      PBS_authenticate - call pbs_iff(1) to authenticate use to the PBS server.
 *
 * @note
 * This function now accepts a argument sock_port and invoke pbs_iff
 * passing this port as a command line argument (both on unix and windows)
 * This change is done because getsockname() fails sometimes on Windows.
 *
 * Also, this would create an environment variable PBS_IFF_CLIENT_ADDR set to
 * the client's connecting address, which is made known to the pbs_iff process.
 *
 * If unable to authenticate, an attempt is made to run the old method
 * 'pbs_iff -i <pbs_client_addr>' also.
 *
 *
 * @param[in]  psock           Socket descriptor used by PBS client to connect PBS server.
 * @param[in]  server_name     Connecting PBS server host name.
 * @param[in]  server_port     Connecting PBS server port number.
 * @param[in]  paddr           Connecting PBS client sockaddr.
 *
 * @return int
 * @retval  0 on success.
 * @retval -1 on failure.
 */
static int
PBSD_authenticate(int psock, char * server_name, int server_port,
	struct sockaddr_in *paddr)
{
	char   cmd[2][PBS_MAXSERVERNAME + 80];
	int    cred_type;
	int    i, k;
	char*  pbs_client_addr = NULL;
	u_short psock_port = 0;
	int	rc;
#ifdef WIN32
	struct	pio_handles	pio;
#else
	FILE	*piff;
#endif
	if (paddr == NULL) {
		return (-1);
	}
	pbs_client_addr = inet_ntoa(paddr->sin_addr);
	if (pbs_client_addr == NULL) {
		return (-1);
	}
	psock_port = paddr->sin_port;

	/* for compatibility with 12.0 pbs_iff */
	(void)snprintf(cmd[1], sizeof(cmd[1])-1, "%s -i %s %s %u %d %u",
		pbs_conf.iff_path, pbs_client_addr,
		server_name, server_port, psock, psock_port);
#ifdef WIN32

	(void)snprintf(cmd[0], sizeof(cmd[0])-1, "%s %s %u %d %u", pbs_conf.iff_path,
		server_name, server_port, psock, psock_port);
	for (k=0; k < 2; k++) {
		rc = 0;
		SetEnvironmentVariable(PBS_IFF_CLIENT_ADDR, pbs_client_addr);
		if (!win_popen(cmd[k], "r", &pio, NULL)) {
			printf("failed to execute %s\n", cmd[k]);
			SetEnvironmentVariable(PBS_IFF_CLIENT_ADDR, NULL);
			rc = -1;
			break;
		}
		i=win_pread(&pio, (char *)&cred_type, (int)sizeof(int));
		win_pclose(&pio);
		SetEnvironmentVariable(PBS_IFF_CLIENT_ADDR, NULL);

		if ((i != sizeof(int)) ||
			(cred_type != PBS_credentialtype_none)) {
			rc = -1;
		} else {
			break;
		}
	}

#else	/* UNIX code here */
	/* Use pbs_iff to authenticate me */
	(void)snprintf(cmd[0], sizeof(cmd[0])-1, "%s=%s %s %s %u %d %u", PBS_IFF_CLIENT_ADDR,
		pbs_client_addr, pbs_conf.iff_path,
		server_name, server_port, psock, psock_port);

	for (k=0; k < 2; k++) {
		rc = 0;

		piff = (FILE *)popen(cmd[k], "r");
		if (piff == NULL) {
			rc = -1;
			break;
		}

		while ((i = read(fileno(piff), &cred_type, sizeof(int))) == -1) {
			if (errno != EINTR)
				break;
		}

		(void)pclose(piff);
		if ((i != sizeof(int)) ||
			(cred_type != PBS_credentialtype_none)) {
			rc = -1;
		} else {
			break;
		}
	}
#endif	/* end of UNIX code */

	return rc;
}

/**
 * @breif
 *	-engage_authentication - Uses the "CS" security library interface to
 * 	do the authentication process that was specified at build time.
 *
 * @param[in]	sd              socket descriptor for this end of a connection
 * @param[in]	server_name     PBS server hostname.
 * @param[in]	server_port     PBS server port number to connect.
 * @param[in]	clnt_paddr      pointer to a client "struct sockaddr_in" variable
 *
 * @return	int
 * @retval	 0  successful
 * @retval	-1 unsuccessful
 *
 * @par	Remark:\n
 *	If the authentication fails, messages are logged to
 *      stderr (cs_logerr) and the mechanism for security context
 *      information is closed (freed).
 *
 */
/* This function will now accept a argument sock_port and will pass it to
 * PBSD_authenticate, which will invoke pbs_iff passing this port as a command
 * line argument. (both on unix and windows)
 * This change is done because getsockname() fails sometimes on Windows.
 */
static int
engage_authentication(int sd,
	char *server_name,
	int  server_port,
	struct sockaddr_in *clnt_paddr)
{
	int	ret;
	char errbuf[ERR_BUF_SIZE];
	char	ebuf[PBS_MAXHOSTNAME + PBS_MAXPORTNUM + 128] = {'\0'};
	if ((sd < 0) || (clnt_paddr == NULL)) {
		cs_logerr(-1, __func__, "Bad arguments, unable to authenticate.");
		return (-1);
	}

	switch (pbs_conf.auth_method) {
		case AUTH_MUNGE:
			if ((ret = engage_external_authentication(sd, AUTH_MUNGE, 0, errbuf, sizeof(errbuf))) != 0)
				cs_logerr(-1, __func__, errbuf);
			return (ret);

		case AUTH_RESV_PORT:
			if ((ret = CS_client_auth(sd)) == CS_SUCCESS)
				return (0);

			if ((ret == CS_AUTH_USE_IFF)) {
				/* CS_client_auth that got called was the one for STD security */
				/*sock_port needs to be passed only for Windows.*/
				if (PBSD_authenticate(sd, server_name, server_port, clnt_paddr) == 0)
					return (0);
			}
			break;

		default:
			cs_logerr(-1, __func__, "Unrecognized authentication method");
			return (-1);
	}

	sprintf(ebuf, "Unable to authenticate connection (%s:%d)", server_name, server_port);
	cs_logerr(-1, __func__, ebuf);
	/* Remove any associated per-connection security context
	 * remark: when using pbs_iff security there is none
	 */

	if (CS_close_socket(sd) != CS_SUCCESS) {
		sprintf(ebuf, "Problem closing context (%s:%d)", server_name, server_port);
		cs_logerr(-1, __func__, ebuf);
	}
	return (-1);
}

/**
 * @brief
 *	-hostnmcmp - compare two hostnames, allowing a short name to match a longer
 *	version of the same
 *
 * @param[in] s1 - hostname1
 * @param[in] s2 - hostname2
 *
 * @return	int
 * @retval	1	success
 * @retval	0	failure
 *
 */
static int
hostnmcmp(char *s1, char *s2)
{
	/* Return failure if any/both the names are NULL. */
	if (s1 == NULL || s2 == NULL)
		return 1;
#ifdef WIN32
	/* Return success if both names are names of localhost. */
	if (is_local_host(s1) && is_local_host(s2))
		return 0;
#endif
	while (*s1 && *s2) {
		if (tolower((int)*s1++) != tolower((int)*s2++))
			return 1;
	}
	if (*s1 == *s2)
		return 0;
	else if ((*s1 == '\0') && ((*s2 == '.') || (*s2 == ':')))
		return 0;
	else if ((*s2 == '\0') && ((*s1 == '.') || (*s1 == ':')))
		return 0;

	return 1;
}

/**
 * @brief
 *	Get the socket fd associated with the connection handle
 *
 * @param[in]   sd - The connection handle
 *
 * @return      Socket descriptor associated with the handle
 *
 */
int
pbs_connection_getsocket(int sd)
{
	return (connection[sd].ch_socket);
}

/**
 * @brief
 *      Generate munge key specific to the user and send PBS batch request
 *      (PBS_BATCH_AuthExternal)to PBS server to authenticate user.
 *
 * @param[in] sock - socket fd
 * @param[in] auth_type - Authentication type (Munge/AMS etc)
 * @param[in] fromsvr - connection initiated from server?
 *
 * @return  int
 * @retval   0 on success
 * @retval  -1 on failure
 * @retval  -2 on unsupported auth_type
 *
 */
int
engage_external_authentication(int sock, int auth_type, int fromsvr, char *ebuf, int ebufsz)
{
	int cred_len = 0, rc = 0, ret = 0;
	char *cred = NULL;
	struct batch_reply *reply = NULL;

	switch (auth_type) {
#ifndef WIN32
		case AUTH_MUNGE:
			ebuf[0] = '\0';
			cred = pbs_get_munge_auth_data(fromsvr, ebuf, ebufsz);
			if (!cred)
				goto err;
			break;
#endif
		default:
			snprintf(ebuf, ebufsz, "Authentication type not supported");
			ret = -2;
	}

	if (cred) {
		ret = -1;
		cred_len = strlen(cred);
		DIS_tcp_setup(sock);
		if (encode_DIS_ReqHdr(sock, PBS_BATCH_AuthExternal, pbs_current_user) ||
				diswuc(sock, auth_type) || /* authentication_type */
				diswsi(sock, cred_len) ||       /* credential length */
				diswcs(sock, cred, cred_len) || /* credential data */
				encode_DIS_ReqExtend(sock, NULL)) {
			pbs_errno = PBSE_SYSTEM;
			goto err;
		}

		if (DIS_tcp_wflush(sock)) {
			pbs_errno = PBSE_SYSTEM;
			goto err;
		}

		memset(cred, 0, cred_len);

		reply = PBSD_rdrpy_sock(sock, &rc);
		if ((reply != NULL) && (reply->brp_code != 0)) {
			pbs_errno = PBSE_BADCRED;
			PBSD_FreeReply(reply);
			goto err;
		}

		PBSD_FreeReply(reply);
		free(cred);
		return 0;
	}

	/* else fall through */

err:
	if (ebuf[0] != '\0') {
		fprintf(stderr, "%s\n", ebuf);
		cs_logerr(-1, __func__, ebuf);
	}
	free(cred);
	return ret;
}

/**
 * @brief
 *	Return the IP address used in binding a socket to a host
 *	Attempts to find IPv4 address for the named host,  first address found
 *	is returned.
 *
 * @param[in]	host - The name of the host to whose address is needed
 * @param[out]	sap  - pointer to the sockaddr_in structure into which
 *						the address will be returned.
 *
 * @return	int
 * @retval  0	- success, address set in *sap
 * @retval -1	- error, *sap is left zero-ed
 */
static int
get_hostsockaddr(char *host, struct sockaddr_in *sap)
{
	struct addrinfo hints;
	struct addrinfo *aip, *pai;

	memset(sap, 0, sizeof(struct sockaddr));
	memset(&hints, 0, sizeof(struct addrinfo));
	/*
	 *	Why do we use AF_UNSPEC rather than AF_INET?  Some
	 *	implementations of getaddrinfo() will take an IPv6
	 *	address and map it to an IPv4 one if we ask for AF_INET
	 *	only.  We don't want that - we want only the addresses
	 *	that are genuinely, natively, IPv4 so we start with
	 *	AF_UNSPEC and filter ai_family below.
	 */
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(host, NULL, &hints, &pai) != 0) {
		pbs_errno = PBSE_BADHOST;
		return -1;
	}
	for (aip = pai; aip != NULL; aip = aip->ai_next) {
		/* skip non-IPv4 addresses */
		if (aip->ai_family == AF_INET) {
			*sap = *((struct sockaddr_in *) aip->ai_addr);
			freeaddrinfo(pai);
			return 0;
		}
	}
	/* treat no IPv4 addresses as getaddrinfo() failure */
	pbs_errno = PBSE_BADHOST;
	freeaddrinfo(pai);
	return -1;
}

/**
 * @brief
 *	Makes a PBS_BATCH_Connect request to 'server'.
 *
 * @param[in]   server - the hostname of the pbs server to connect to.
 * @param[in]   extend_data - a string to send as "extend" data.
 *
 * @return int
 * @retval >= 0	index to the internal connection table representing the
 *		connection made.
 * @retval -1	error encountered setting up the connection.
 */
int
__pbs_connect_extend(char *server, char *extend_data)
{
	struct sockaddr_in server_addr;
	struct sockaddr_in my_sockaddr;
	int out;
	int i;
	int f;
	char  *altservers[2];
	int    have_alt = 0;
	struct batch_reply	*reply;
	char server_name[PBS_MAXSERVERNAME+1];
	unsigned int server_port;
	struct sockaddr_in sockname;
	pbs_socklen_t	 socknamelen;
#ifdef WIN32
	struct sockaddr_in to_sock;
	struct sockaddr_in from_sock;
#endif

#ifndef WIN32
	char   pbsrc[_POSIX_PATH_MAX];
	struct stat sb;
	int    using_secondary = 0;
#endif  /* not WIN32 */

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return -1;

	if (pbs_loadconf(0) == 0)
		return -1;

	/* get server host and port	*/

	server = PBS_get_server(server, server_name, &server_port);
	if (server == NULL) {
		pbs_errno = PBSE_NOSERVER;
		return -1;
	}

	if (pbs_conf.pbs_primary && pbs_conf.pbs_secondary) {
		/* failover configuered ...   */
		if (hostnmcmp(server, pbs_conf.pbs_primary) == 0) {
			have_alt = 1;
			/* We want to try the one last seen as "up" first to not   */
			/* have connection delays.   If the primary was up, there  */
			/* is no .pbsrc.NAME file.  If the last command connected  */
			/* to the Secondary, then it created the .pbsrc.USER file. */

			/* see if already seen Primary down */
#ifdef WIN32
			/* due to windows quirks, all try both in same order */
			altservers[0] = pbs_conf.pbs_primary;
			altservers[1] = pbs_conf.pbs_secondary;
#else
			(void)snprintf(pbsrc, _POSIX_PATH_MAX, "%s/.pbsrc.%s", pbs_conf.pbs_tmpdir, pbs_current_user);
			if (stat(pbsrc, &sb) == -1) {
				/* try primary first */
				altservers[0] = pbs_conf.pbs_primary;
				altservers[1] = pbs_conf.pbs_secondary;
				using_secondary = 0;
			} else {
				/* try secondary first */
				altservers[0] = pbs_conf.pbs_secondary;
				altservers[1] = pbs_conf.pbs_primary;
				using_secondary = 1;
			}
#endif
		}
	}

	/* if specific host name declared for the host on which */
	/* this client is running,  get its address */
	if (pbs_conf.pbs_public_host_name) {
		if (get_hostsockaddr(pbs_conf.pbs_public_host_name, &my_sockaddr) != 0)
			return -1; /* pbs_errno was set */
	}

	/* Reserve a connection state record */
	if (pbs_client_thread_lock_conntable() != 0)
		return -1;

	out = -1;
	for (i=1;i<NCONNECTS;i++) {
		if (connection[i].ch_inuse) continue;
		out = i;
		connection[out].ch_errno = 0;
		connection[out].ch_socket= -1;
		connection[out].ch_errtxt = NULL;
		connection[out].ch_inuse = 1; /* reserve the socket */
		break;
	}

	if (pbs_client_thread_unlock_conntable() != 0)
		return -1; /* pbs_errno set by the function */

	if (out < 0) {
		pbs_errno = PBSE_NOCONNECTS;
		return -1;
	}

	/*
	 * connect to server ...
	 * If attempt to connect fails and if Failover configured and
	 *   if attempting to connect to Primary,  try the Secondary
	 *   if attempting to connect to Secondary, try the Primary
	 */
	for (i=0; i<(have_alt+1); ++i) {

		/* get socket	*/

#ifdef WIN32
		/* the following lousy hack is needed since the socket call needs */
		/* SYSTEMROOT env variable properly set! */
		if (getenv("SYSTEMROOT") == NULL) {
			setenv("SYSTEMROOT", "C:\\WINNT", 1);
			setenv("SystemRoot", "C:\\WINNT", 1);
		}
		connection[out].ch_socket = socket(AF_INET, SOCK_STREAM, 0);
		if (connection[out].ch_socket < 0) {
			setenv("SYSTEMROOT", "C:\\WINDOWS", 1);
			setenv("SystemRoot", "C:\\WINDOWS", 1);
			connection[out].ch_socket = socket(AF_INET, SOCK_STREAM, 0);

		}
#else
		connection[out].ch_socket = socket(AF_INET, SOCK_STREAM, 0);
#endif
		if (connection[out].ch_socket < 0) {
			connection[out].ch_inuse = 0;
			pbs_errno = errno;
			return -1;
		}

		/* and connect... */

		if (have_alt) {
			server = altservers[i];
		}
		strcpy(pbs_server, server); /* set for error messages from commands */

		/* If a specific host name is defined which the client should use */

		if (pbs_conf.pbs_public_host_name) {
			/* my address will be in my_sockaddr,  bind the socket to it */
			my_sockaddr.sin_port = 0;
			if (bind(connection[out].ch_socket, (struct sockaddr *)&my_sockaddr, sizeof(my_sockaddr)) != 0) {
				return -1;
			}
		}

		if (get_hostsockaddr(server, &server_addr) != 0)
			return -1;

		server_addr.sin_port = htons(server_port);
		if (connect(connection[out].ch_socket,
			(struct sockaddr *)&server_addr,
			sizeof(struct sockaddr)) == 0) {

				break;
		} else {
			/* connect attempt failed */
			CLOSESOCKET(connection[out].ch_socket);
			pbs_errno = errno;
		}
	}
	if (i >= (have_alt+1)) {
		connection[out].ch_inuse = 0;
		return -1; 		/* cannot connect */
	}

#ifndef WIN32
	if (have_alt && (i == 1)) {
		/* had to use the second listed server ... */
		if (using_secondary == 1) {
			/* remove file that causes trying the Secondary first */
			unlink(pbsrc);
		} else {
			/* create file that causes trying the Primary first   */
			f = open(pbsrc, O_WRONLY|O_CREAT, 0200);
			if (f != -1)
				(void)close(f);
		}
	}
#endif

	/* setup connection level thread context */
	if (pbs_client_thread_init_connect_context(out) != 0) {
		CLOSESOCKET(connection[out].ch_socket);
		connection[out].ch_inuse = 0;
		/* pbs_errno set by the pbs_connect_init_context routine */
		return -1;
	}

	/*
	 * No need for global lock now on, since rest of the code
	 * is only communication on a connection handle.
	 * But we dont need to lock the connection handle, since this
	 * connection handle is not yet been returned to the client
	 */

	/* The following code was originally  put in for HPUX systems to deal
	 * with the issue where returning from the connect() call doesn't
	 * mean the connection is complete.  However, this has also been
	 * experienced in some Linux ppc64 systems like js-2. Decision was
	 * made to enable this harmless code for all architectures.
	 * FIX: Need to use the socket to send
	 * a message to complete the process.  For IFF authentication there is
	 * no leading authentication message needing to be sent on the client
	 * socket, so will send a "dummy" message and discard the replyback.
	 */

#if !defined(PBS_SECURITY ) || (PBS_SECURITY == STD )

	DIS_tcp_setup(connection[out].ch_socket);
	if ((i = encode_DIS_ReqHdr(connection[out].ch_socket,
		PBS_BATCH_Connect, pbs_current_user)) ||
		(i = encode_DIS_ReqExtend(connection[out].ch_socket,
		extend_data))) {
		pbs_errno = PBSE_SYSTEM;
		return -1;
	}
	if (DIS_tcp_wflush(connection[out].ch_socket)) {
		pbs_errno = PBSE_SYSTEM;
		return -1;
	}

	reply = PBSD_rdrpy(out);
	PBSD_FreeReply(reply);

#endif	/* PBS_SECURITY ... */

	/*do configured authentication (kerberos, pbs_iff, whatever)*/

	/*Get the socket port for engage_authentication() */
	socknamelen = sizeof(sockname);
	if (getsockname(connection[out].ch_socket, (struct sockaddr *)&sockname, &socknamelen))
		return -1;

	if (engage_authentication(connection[out].ch_socket,
		server,
		server_port,
		&sockname) == -1) {
		CLOSESOCKET(connection[out].ch_socket);
		connection[out].ch_inuse = 0;
		pbs_errno = PBSE_PERM;
		return -1;
	}

	/* setup DIS support routines for following pbs_* calls */

	DIS_tcp_setup(connection[out].ch_socket);
	pbs_tcp_timeout = PBS_DIS_TCP_TIMEOUT_VLONG;	/* set for 3 hours */

	return out;
}

/**
 * @brief
 *	Set no-delay option (disable nagles algoritm) on connection
 *
 * @param[in]   connect - connection index
 *
 * @return int
 * @retval  0	Succcess
 * @retval -1	Failure (bad index, or failed to set)
 *
 */
int
pbs_connection_set_nodelay(int connect)
{
	int fd;
	int opt;
	pbs_socklen_t optlen;

	if (connect < 0 || NCONNECTS <= connect)
		return -1;

	if (!connection[connect].ch_inuse)
		return -1;

	optlen = sizeof(opt);
	fd = connection[connect].ch_socket;
	if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, &optlen) == -1)
		return -1;

	if (opt == 1)
		return 0;

	opt = 1;
	return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

/**
 * @brief
 *	A wrapper progarm to pbs_connect_extend() but this one not
 *	passing any 'extend' data to the connection.
 *
 * @param[in] server - server - the hostname of the pbs server to connect to.
 *
 * @retval int	- return value of pbs_connect_extend().
 */
int
__pbs_connect(char *server)
{
	return (pbs_connect_extend(server, NULL));
}

/**
 * @brief
 *	-send close connection batch request
 *
 * @param[in] connect - socket descriptor
 *
 * @return	int
 * @retval	0	success
 * @retval	-1	error
 *
 */
int
__pbs_disconnect(int connect)
{
	int  sock;
	char x;

	if (connect < 0 || NCONNECTS <= connect)
		return 0;

	if (!connection[connect].ch_inuse)
		return 0;

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return -1;

	/*
	 * Use only connection handle level lock since this is
	 * just communication with server
	 */
	if (pbs_client_thread_lock_connection(connect) != 0)
		return -1;

	/*
	 * check again to ensure that another racing thread
	 * had not already closed the connection
	 */
	if (!connection[connect].ch_inuse) {
		(void)pbs_client_thread_unlock_connection(connect);
		return 0;
	}

	/* send close-connection message */

	sock = connection[connect].ch_socket;

	DIS_tcp_setup(sock);
	if ((encode_DIS_ReqHdr(sock, PBS_BATCH_Disconnect,
		pbs_current_user) == 0) &&
		(DIS_tcp_wflush(sock) == 0)) {
		for (;;) {	/* wait for server to close connection */
#ifdef WIN32
			if (recv(sock, &x, 1, 0) < 1)
#else
			if (read(sock, &x, 1) < 1)
#endif
				break;
		}
	}

	CS_close_socket(sock);
	CLOSESOCKET(sock);

	if (connection[connect].ch_errtxt != NULL) {
		free(connection[connect].ch_errtxt);
		connection[connect].ch_errtxt = NULL;
	}
	connection[connect].ch_errno = 0;
	connection[connect].ch_inuse = 0;

	/* unlock the connection level lock */
	if (pbs_client_thread_unlock_connection(connect) != 0)
		return -1;

	/*
	 * this is only a per thread work, so outside lock and unlock
	 * connection needs the thread level connect context so this should be
	 * called after unlocking
	 */
	if (pbs_client_thread_destroy_connect_context(connect) != 0)
		return -1;

	return 0;
}

/**
 * @brief
 *	-return the number of max connections.
 *
 * @return	int
 * @retval	0	success
 * @retval	!0	error
 *
 */
int
pbs_query_max_connections()
{
	return (NCONNECTS - 1);
}

/*
 *	pbs_connect_noblk() - Open a connection with a pbs server.
 *		Do not allow TCP to block us if Server host is down
 *
 *	At this point, this does not attempt to find a fail_over Server
 */

/**
 * @brief
 *	Open a connection with a pbs server.
 *	Do not allow TCP to block us if Server host is down
 *	At this point, this does not attempt to find a fail_over Server
 *
 * @param[in]   server - specifies the server to which to connect
 * @param[in]   tout - timeout value for select
 *
 * @return int
 * @retval >= 0	index to the internal connection table representing the
 *		connection made.
 * @retval -1	error encountered in getting index
 */
int
pbs_connect_noblk(char *server, int tout)
{
	int out;
	int i;
	pbs_socklen_t l;
	int n;
	struct timeval tv;
	fd_set fdset;
	struct batch_reply *reply;
	char server_name[PBS_MAXSERVERNAME+1];
	unsigned int server_port;
	struct addrinfo *aip, *pai;
	struct addrinfo hints;
	struct sockaddr_in *inp;
	short int connect_err = 0;

	struct sockaddr_in sockname;
	pbs_socklen_t	 socknamelen;

#ifdef WIN32
	int     non_block = 1;
	struct sockaddr_in to_sock;
	struct sockaddr_in from_sock;
#endif

#ifndef WIN32
	int nflg;
	int oflg;
#endif

	/* initialize the thread context data, if not already initialized */
	if (pbs_client_thread_init_thread_context() != 0)
		return -1;

	if (pbs_loadconf(0) == 0)
		return -1;

	/* get server host and port	*/

	server = PBS_get_server(server, server_name, &server_port);
	if (server == NULL) {
		pbs_errno = PBSE_NOSERVER;
		return -1;
	}

	/* Reserve a connection state record */
	if (pbs_client_thread_lock_conntable() != 0)
		return -1;

	out = -1;
	for (i=1;i<NCONNECTS;i++) {
		if (connection[i].ch_inuse) continue;
		out = i;
		connection[out].ch_inuse = 1;
		connection[out].ch_errno = 0;
		connection[out].ch_socket= -1;
		connection[out].ch_errtxt = NULL;
		break;
	}

	if (pbs_client_thread_unlock_conntable() != 0)
		return -1; /* pbs_errno set by the function */

	if (out < 0) {
		pbs_errno = PBSE_NOCONNECTS;
		return -1;
	}


	/* get socket	*/

#ifdef WIN32
	/* the following lousy hack is needed since the socket call needs */
	/* SYSTEMROOT env variable properly set! */
	if (getenv("SYSTEMROOT") == NULL) {
		setenv("SYSTEMROOT", "C:\\WINNT", 1);
		setenv("SystemRoot", "C:\\WINNT", 1);
	}
	connection[out].ch_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (connection[out].ch_socket < 0) {
		setenv("SYSTEMROOT", "C:\\WINDOWS", 1);
		setenv("SystemRoot", "C:\\WINDOWS", 1);
		connection[out].ch_socket = socket(AF_INET, SOCK_STREAM, 0);
	}
#else
	connection[out].ch_socket = socket(AF_INET, SOCK_STREAM, 0);
#endif
	if (connection[out].ch_socket < 0) {
		connection[out].ch_inuse = 0;
		pbs_errno = ERRORNO;
		return -1;
	}

	/* set socket non-blocking */

#ifdef WIN32
	if (ioctlsocket(connection[out].ch_socket, FIONBIO, &non_block) == SOCKET_ERROR)
#else
	oflg = fcntl(connection[out].ch_socket, F_GETFL) & ~O_ACCMODE;
	nflg = oflg | O_NONBLOCK;
	if (fcntl(connection[out].ch_socket, F_SETFL, nflg) == -1)
#endif
		goto err;

	/* and connect... */

	strcpy(pbs_server, server);    /* set for error messages from commands */
	memset(&hints, 0, sizeof(struct addrinfo));
	/*
	 *      Why do we use AF_UNSPEC rather than AF_INET?  Some
	 *      implementations of getaddrinfo() will take an IPv6
	 *      address and map it to an IPv4 one if we ask for AF_INET
	 *      only.  We don't want that - we want only the addresses
	 *      that are genuinely, natively, IPv4 so we start with
	 *      AF_UNSPEC and filter ai_family below.
	 */
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	if (getaddrinfo(server, NULL, &hints, &pai) != 0) {
		CLOSESOCKET(connection[out].ch_socket);
		connection[out].ch_inuse = 0;
		pbs_errno = PBSE_BADHOST;
		return -1;
	}
	for (aip = pai; aip != NULL; aip = aip->ai_next) {
		/* skip non-IPv4 addresses */
		if (aip->ai_family == AF_INET) {
			inp = (struct sockaddr_in *) aip->ai_addr;
			break;
		}
	}
	if (aip == NULL) {
		/* treat no IPv4 addresses as getaddrinfo() failure */
		CLOSESOCKET(connection[out].ch_socket);
		connection[out].ch_inuse = 0;
		pbs_errno = PBSE_BADHOST;
		freeaddrinfo(pai);
		return -1;
	} else
		inp->sin_port = htons(server_port);
	if (connect(connection[out].ch_socket,
		aip->ai_addr,
		aip->ai_addrlen) < 0) {
		connect_err = 1;
	}
	if (connect_err == 1)
	{
		/* connect attempt failed */
		pbs_errno = ERRORNO;
		switch (pbs_errno) {
#ifdef WIN32
			case WSAEWOULDBLOCK:
#else
			case EINPROGRESS:
			case EWOULDBLOCK:
#endif
				while (1) {
					FD_ZERO(&fdset);
					FD_SET(connection[out].ch_socket, &fdset);
					tv.tv_sec = tout;
					tv.tv_usec = 0;
					n = select(connection[out].ch_socket+1, NULL, &fdset, NULL, &tv);
					if (n > 0) {
						pbs_errno = 0;
						l = sizeof(pbs_errno);
						(void)getsockopt(connection[out].ch_socket,
							SOL_SOCKET, SO_ERROR,
							&pbs_errno, &l);
						if (pbs_errno == 0)
							break;
						else
							goto err;
					} if ((n < 0) &&
#ifdef WIN32
						(ERRORNO == WSAEINTR)
#else
						(ERRORNO == EINTR)
#endif
						) {
						continue;
					} else {
						goto err;
					}
				}
				break;

			default:
err:
				CLOSESOCKET(connection[out].ch_socket);
				connection[out].ch_inuse = 0;
				freeaddrinfo(pai);
				return -1;	/* cannot connect */

		}
	}
	freeaddrinfo(pai);

	/* reset socket blocking */
#ifdef WIN32
	non_block = 0;
	if (ioctlsocket(connection[out].ch_socket, FIONBIO, &non_block) == SOCKET_ERROR)
#else
	if (fcntl(connection[out].ch_socket, F_SETFL, oflg) < 0)
#endif
		goto err;

	/*
	 * multiple threads cant get the same connection id above,
	 * so no need to lock this piece of code
	 */
	/* setup connection level thread context */
	if (pbs_client_thread_init_connect_context(out) != 0) {
		CLOSESOCKET(connection[out].ch_socket);
		connection[out].ch_inuse = 0;
		/* pbs_errno set by the pbs_connect_init_context routine */
		return -1;
	}
	/*
	 * even though the following is communication with server on
	 * a connection handle, it does not need to be lock since
	 * this connection handle has not be returned back yet to the client
	 * so others threads cannot use it
	 */

	/* send "dummy" connect message */
	DIS_tcp_setup(connection[out].ch_socket);
	if ((i = encode_DIS_ReqHdr(connection[out].ch_socket,
		PBS_BATCH_Connect, pbs_current_user)) ||
		(i = encode_DIS_ReqExtend(connection[out].ch_socket,
		NULL))) {
		pbs_errno = PBSE_SYSTEM;
		return -1;
	}
	if (DIS_tcp_wflush(connection[out].ch_socket)) {
		pbs_errno = PBSE_SYSTEM;
		return -1;
	}
	reply = PBSD_rdrpy(out);
	PBSD_FreeReply(reply);

	/*do configured authentication (kerberos, pbs_iff, whatever)*/

	/*Get the socket port for engage_authentication()*/
	socknamelen = sizeof(sockname);
	if (getsockname(connection[out].ch_socket, (struct sockaddr *)&sockname, &socknamelen))
		return -1;
	if (engage_authentication(connection[out].ch_socket,
		server,
		server_port,
		&sockname) == -1) {
		CLOSESOCKET(connection[out].ch_socket);
		connection[out].ch_inuse = 0;
		pbs_errno = PBSE_PERM;
		return -1;
	}

	/* setup DIS support routines for following pbs_* calls */
	DIS_tcp_setup(connection[out].ch_socket);
	pbs_tcp_timeout = PBS_DIS_TCP_TIMEOUT_VLONG;	/* set for 3 hours */

	return out;
}
