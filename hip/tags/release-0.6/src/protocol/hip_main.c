/*
 * Host Identity Protocol
 * Copyright (C) 2002-06 the Boeing Company
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
 *  hip_main.c
 *
 *  Authors: Jeff Ahrenholz <jeffrey.m.ahrenholz@boeing.com>
 *           Tom Henderson <thomas.r.henderson@boeing.com>
 * 
 * Main program for HIP daemon.
 *
 */

/*
 * Style:  KNF where possible, K&R style braces around control structures
 * Style:  indent using tabs-- multi-line continuation 4 spaces
 * Style:  no tabs in middle of lines
 * Style:  this code authored with tabstop=8
 */

#include <stdio.h>           /* stderr, etc                  */
#include <stdlib.h>          /* rand()                       */
#include <errno.h>           /* strerror(), errno            */
#include <string.h>          /* memset()                     */
#include <time.h>            /* time()                       */
#include <ctype.h>           /* tolower()                    */
#include <fcntl.h>
#ifdef SMA_CRAWLER
#include <utime.h>
#include <sys/resource.h>    /* getrlimit, setrlimit         */
#endif
#ifdef __WIN32__
#include <winsock2.h>
#include <ws2tcpip.h>
#include <win32/types.h>
#include <process.h>
#include <io.h>
#else
#include <sys/socket.h>      /* sock(), recvmsg(), etc       */
#include <sys/time.h>        /* gettimeofday()               */
#include <sys/uio.h>		/* iovec */
#include <sys/wait.h>		/* waitpid() */
#include <arpa/inet.h>       /* inet_pton()                  */
#ifdef __MACOSX__
#include <netinet/in_systm.h>
#endif
#include <netinet/in.h>      /* struct sockaddr_in, etc      */
#include <netinet/ip.h>      /* struct iphdr                 */
#include <unistd.h>          /* fork(), getpid()             */
#include <math.h>            /* pow()                        */
#ifdef __UMH__
#include <pthread.h>
#include <netdb.h>
#endif
#endif
#include <sys/types.h>       /* getpid() support, etc        */
#include <signal.h>          /* signal()                     */
#include <openssl/crypto.h>  /* OpenSSL's crypto library     */
#include <openssl/bn.h>      /* Big Numbers                  */
#include <openssl/dsa.h>     /* DSA support                  */
#include <openssl/asn1.h>    /* DSAparams_dup()              */
#include <openssl/dh.h>      /* Diffie-Hellman contexts      */
#include <openssl/sha.h>     /* SHA1 algorithms              */
#include <openssl/rand.h>    /* RAND_seed()                  */
#if defined(__UMH__) || defined(__MACOSX__)
#include <win32/pfkeyv2.h>
#else
#include <linux/pfkeyv2.h>	/* PF_KEY_V2 support */
#endif
#include <hip/hip_types.h>
#include <hip/hip_proto.h>
#include <hip/hip_globals.h>
#include <hip/hip_funcs.h>
#ifdef SMA_CRAWLER
#include <hip/hip_cfg_api.h>
#endif

#ifdef HIP_I3
#include "i3_hip.h"
int (*select_func)(int, fd_set*, fd_set*, fd_set*, struct timeval *);
#endif

#ifdef __MACOSX__
extern void del_divert_rule(int);
#endif

#ifndef __MACOSX__
#ifndef __WIN32__
#define IPV6_HIP
#endif
#endif
/*
 * Function prototypes
 */
int main(int argc, char *argv[]);

/* HIP packets */
/*I3: static */
#ifdef __WIN32__
void hip_handle_packet(__u8* buff, int length, struct sockaddr *src, int use_udp);
#else
void hip_handle_packet(struct msghdr *msg, int length, __u16 family, int use_udp);
#endif
void hip_handle_state_timeouts(struct timeval *time1);
void hip_handle_locator_state_timeouts(hip_assoc *hip_a, struct timeval *time1);
void hip_handle_registrations(struct timeval *time1);
static void hip_retransmit_waiting_packets(struct timeval *time1);
int hip_trigger(struct sockaddr *dst);
int hip_trigger_rvs(struct sockaddr*rvs, hip_hit *responder);

#ifdef __UMH__
#ifndef __CYGWIN__
#ifndef __WIN32__
void post_init_tap();
#endif
#endif
#endif
#ifdef SMA_CRAWLER
void endbox_init();
int hipcfg_init();
extern __u32 get_preferred_lsi(struct sockaddr *);
#endif

#include <hip/hip_stun.h>

/*
 * main():  HIP daemon main event loop
 *     - read command line options
 *     - read configuration file
 *     - crypto init -- generate Diffie Hellman material
 *     - generate R1s 
 *     - some timer for timeout activies (rotate R1, expire states)
 *     - create HIP and PFKEY sockets
 *     - go to endless loop, selecting on the sockets
 */

int main_loop(int argc, char **argv)
{
	struct timeval time1, timeout; /* Used in select() loop */

	/* HIP and PFKEY socket data structures */
#ifndef __WIN32__
	struct msghdr msg;
	struct iovec iov;
#endif
	struct sockaddr_in addr, addr_udp; /* For IPv4 */

	struct sockaddr_storage addr_from;
	__u32 addr_from_len;
	fd_set read_fdset;
	char buff[2048];
#ifdef IPV6_HIP
	struct sockaddr_in6 addr6; /* For IPv6 */
	int optval=1;
#ifndef __WIN32__
	char cbuff[CMSG_SPACE(256)];
#endif
#endif
	int num_icmp_errors=0;
	int highest_descriptor=0;
	int flags=0, err=0, length=0, last_expire=0, i;
	int need_select_preferred=FALSE;
#ifdef SMA_CRAWLER
        time_t last_time, now_time;
        int ret;
        struct rlimit limits;
#endif
#ifndef __MACOSX__
	NatType nattype; /* used for NAT detection with STUN */
	int optval_udp = 1;
#endif

	/* Predefining global variables */
	memset(hip_assoc_table, 0, sizeof(hip_assoc_table));
	memset(hip_mr_client_table, 0, sizeof(hip_mr_client_table));
	memset(hip_reg_table, 0, sizeof(hip_reg_table));
	fr.add_from = FALSE;
	fr2.add_via_rvs = FALSE;

#ifndef __UMH__
	/* Initialize OpenSSL crypto library, if not already done 
	 * in init_hip() */
	init_crypto();
#endif

	/* 
	 * Set default options
	 * later modified by command-line parameters
	 */
	memset(&OPT, 0, sizeof(struct hip_opt));
	OPT.daemon = FALSE;
	OPT.debug = D_DEFAULT;
	OPT.debug_R1 = D_QUIET;
	OPT.no_retransmit = FALSE;
	OPT.permissive = FALSE;
	OPT.opportunistic = FALSE;
	OPT.allow_any = FALSE;
	OPT.enable_udp = FALSE;
	OPT.trigger = NULL;
	OPT.use_i3 = FALSE;
	OPT.rvs = FALSE;
	OPT.stun = FALSE;
	OPT.mr = FALSE;
	
	/*
	 * Set default configuration
	 * later modified by command-line parameters or conf file
	 */
	memset(&HCNF, 0, sizeof(struct hip_conf));
	HCNF.cookie_difficulty = 10;
	HCNF.cookie_lifetime = 39; /* 2^(39-32) = 2^7 = 128 seconds */
	HCNF.packet_timeout = 5;
	HCNF.max_retries = 5;
	HCNF.sa_lifetime = 900; /* 15 minutes, as recommended by draft-esp */
	HCNF.loc_lifetime = 1800; /* 30 minutes */
	HCNF.preferred_hi = NULL;
	HCNF.send_hi_name = TRUE;
	HCNF.dh_group = DEFAULT_DH_GROUP_ID;
	HCNF.dh_lifetime = 900;
	HCNF.r1_lifetime = 300;
	HCNF.msl = 5;
	HCNF.ual = 600;
	HCNF.failure_timeout = (HCNF.max_retries * HCNF.packet_timeout);
	for (i = 0; i < (SUITE_ID_MAX - 1); i++)
		HCNF.esp_transforms[i] = HCNF.hip_transforms[i] = (__u16)(i+1);
	HCNF.log_filename = NULL;
	HCNF.disable_dns_lookups = FALSE;
	HCNF.disable_notify = FALSE;
#ifdef __UMH__
	HCNF.disable_dns_thread = FALSE;
	HCNF.enable_bcast = FALSE;
#endif
	HCNF.min_lifetime = 96;  /* min lt offered by rvs: 2^((96-64)/8) = s */
	HCNF.max_lifetime = 255; /* max lt offered by rvs: 2^((255-64)/8) = s */
	HCNF.reg_type_rvs = REG_RVS;   /* registration type offered by the rvs */
	HCNF.lifetime = 255;     /* lt req by non rvs node: 2^((255-64)/8) = s*/
	HCNF.reg_type = REG_RVS;
	HCNF.preferred_iface = NULL;
	HCNF.outbound_iface = NULL;
	HCNF.save_known_identities = TRUE;
	HCNF.peer_certificate_required = FALSE;
	memset(HCNF.conf_filename, 0, sizeof(HCNF.conf_filename));
	memset(HCNF.my_hi_filename, 0, sizeof(HCNF.my_hi_filename));
	memset(HCNF.known_hi_filename, 0, sizeof(HCNF.known_hi_filename));

	/*
	 * check program arguments
	 */
	argv++, argc--;
	while (argc > 0) {
		if (strcmp(*argv, "-v") == 0) {
			OPT.debug = D_VERBOSE;
			argv++, argc--;
			continue;
			
		} else if (strcmp(*argv, "-q") == 0) {
			OPT.debug = D_QUIET;
			OPT.debug_R1 = D_QUIET;
			argv++, argc--;
			continue;
		} else if (strcmp(*argv, "-d") == 0) {
			OPT.daemon = TRUE;
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-r1") == 0) {
			OPT.debug_R1 = OPT.debug;
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-p") == 0) {
			OPT.permissive = TRUE;
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-nr") == 0) {
			OPT.no_retransmit = TRUE;
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-o") == 0) {
			OPT.opportunistic = TRUE;
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-a") == 0) {
			OPT.allow_any = TRUE;
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-t") == 0) {
			int af;
			argv++, argc--;
			if (argc==0 || !argv) {
				log_(ERR, "Please supply a trigger address.\n");
				exit(1);
			}
			af = ((strchr(*argv, ':')==NULL) ? AF_INET : AF_INET6);
			OPT.trigger = (struct sockaddr*)malloc((af==AF_INET) ?
					     sizeof(struct sockaddr_in) :
					     sizeof(struct sockaddr_in6));
			memset(OPT.trigger, 0, sizeof(OPT.trigger));
			OPT.trigger->sa_family = af;
			if (str_to_addr((__u8*)*argv, OPT.trigger) < 1) {
				log_(ERR, "Invalid trigger address.\n");
				exit(1);
			}
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-u") == 0) {
			OPT.enable_udp = TRUE;
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-i3") == 0) {
			OPT.use_i3 = TRUE;
			argv++, argc--;
			continue;
		}
		if (strcmp(*argv, "-conf") == 0) {
			argv++, argc--;
			strncpy(HCNF.conf_filename, *argv, 
				sizeof(HCNF.conf_filename));
			log_(NORM,	"Using user-provided hip.conf file " \
					"location.\n");
			argv++, argc--;
			continue;
		}
		/* Mobile node client */
		if (strcmp(*argv, "-mn") == 0){
			OPT.mn = TRUE;
			argv++,argc--;
			continue;
		}
		/* Mobile router service or rendezvous server */
		if ((strcmp(*argv, "-mr") == 0) || (strcmp(*argv, "-m") == 0)) {
			if (HCNF.n_reg_types >= MAX_REGISTRATION_TYPES){
				log_(ERR, "Error: number of registration "
					"types exceeds %d\n",
					MAX_REGISTRATION_TYPES);
				exit(1);
			}
			if (strcmp(*argv, "-mr") == 0) {
				OPT.mr = TRUE;
				HCNF.reg_types[HCNF.n_reg_types++] = REG_MR;
			} else {
				OPT.rvs = TRUE;
				HCNF.reg_types[HCNF.n_reg_types++] = REG_RVS;
			}
			argv++,argc--;
			continue;
		}
		/* to fill the registration table with a number of entries */
		if (strcmp(*argv, "-g") == 0){
			argv++, argc--;
			if (OPT.rvs == FALSE){
				log_(ERR, "Error while trying to fill the " \
					  "registration table in normal mode " \
					  "(not rvs mode).\n");
				exit(1);
			}
			if (argc==0 || !argv) {
				log_(ERR, "Please supply a number of entries "\
					  "to fill the registration table.\n");
				exit(1);
			}
			OPT.entries = TRUE;
			sscanf(*argv,"%ld", &num_entries);
			argv++, argc--;
			continue;
		}
		
#ifndef __MACOSX__
		/* to use NAT detection, provide a STUN server address */
		if (strcmp(*argv, "-stun") == 0) {
			argv++, argc--;
			if (argc==0 || !argv) {
				log_(ERR, "Please supply a STUN server address.\n");
				exit(1);
			}
			memset(&STUN_server_addr, 0, sizeof (StunAddress4));
			if (stunParseServerName( *argv, &STUN_server_addr)) {
				OPT.stun = TRUE;
			} else {
				log_(ERR, "Bad STUN server address. NAT detection not enabled.\n");
				OPT.stun = FALSE;
			}
			argv++, argc--;
			continue;
		}
#endif

		print_usage();
		exit(1);
	}

#ifndef __UMH__ /* don't mix pthreads with fork() */
	if (OPT.daemon) {
		if (fork() > 0)
			return(0);
		/* TODO: properly daemonize the program here:
		 *  change file mode mask
		 *  setsid() obtain a new process group
		 *  chdir("/")
		 *  replace stdout, stderr, stdin
		 */
	}
#endif
	if (init_log() < 0)
		goto hip_main_error_exit;

#ifdef __WIN32__
	log_(QOUT, "hipd started.\n");
#else
	log_(QOUT, "hipd (%d) started.\n", getpid());
#endif
	log_hipopts();

	/* 
	 * Load hip.conf configuration file
	 * user may have provided path using command line, or search defaults
	 */
	if ((locate_config_file(HCNF.conf_filename, sizeof(HCNF.conf_filename), 
			HIP_CONF_FILENAME) < 0) ||
		(read_conf_file(HCNF.conf_filename) < 0)) {
		log_(ERR, "Problem with configuration file, using defaults.\n");
	} else {
		log_(NORM, "Using configuration file:\t%s\n",
			HCNF.conf_filename);
	}
       
	/*
	 * Load the my_host_identities.xml file.
	 */
	my_hi_head = NULL;
#ifdef SMA_CRAWLER
	hi_node *my_hi;
	if (hipcfg_init("libhipcfg.so", &HCNF)) {
		log_(WARN, "Error loading libhipcfg.so\n");
		goto hip_main_error_exit;
	}
	if (HCNF.use_smartcard) {
		if ((my_hi = hipcfg_getMyHostId()) == NULL) {
			log_(ERR, "Error retrieving host identity from "
				"smartcard\n");
			 goto hip_main_error_exit;
		}
		/* use smartcard for signing */
		append_hi_node(&my_hi_head, my_hi);
	} else {
#endif /* SMA_CRAWLER */
	if ((locate_config_file(HCNF.my_hi_filename,
			sizeof(HCNF.my_hi_filename), HIP_MYID_FILENAME) < 0)) {
		log_(ERR, "Unable to locate this machine's %s file.\n",
			HIP_MYID_FILENAME);
	} else {
		log_(NORM, "Using my host IDs file:\t\t%s\n",
			HCNF.my_hi_filename);
	}
	if (read_identities_file(HCNF.my_hi_filename, TRUE) < 0) {
		log_(ERR, "Problem with my host identities file.\n");
		log_(QOUT, "\n  You must have a valid %s file containing the "
			"identities\n  for this host. You can create this file "
			"using the 'hitgen' utility.\n", HIP_MYID_FILENAME);
		goto hip_main_error_exit; /* fatal error */
	}

	/*
	 * Load the known_host_identities.xml file.
	 */
	peer_hi_head = NULL;
	if ((locate_config_file(HCNF.known_hi_filename,
		sizeof(HCNF.known_hi_filename),	HIP_KNOWNID_FILENAME) < 0)) {
		log_(ERR, "Unable to locate this machine's %s file.\n",
			HIP_KNOWNID_FILENAME);
	} else {
		log_(NORM, "Using known host IDs file:\t%s\n",
			HCNF.known_hi_filename);
	}
	if (read_identities_file(HCNF.known_hi_filename, FALSE) < 0) {
    		log_(ERR, "Problem reading the %s file which is used to "
			"specify\n  peer HITs.\n",
			HIP_KNOWNID_FILENAME);
    		if (!OPT.allow_any)
		    log_(ERR, "Because there are no peer identities, you probab"
			"ly need to run with the -a\n  (allow any) option.\n");
	}


	if (get_preferred_hi(my_hi_head)==NULL) {
		log_(ERR, "The preferred HI specified in %s was not found.\n",
			HIP_CONF_FILENAME);
		goto hip_main_error_exit;
	}

	/* if OPT.entries is activated */
	if (OPT.entries == TRUE) {
		num_hip_reg = num_entries;
		if(read_reg_file() <0)
			log_(ERR, "Problem with the registration file.\n");
		/* Sorts the entries read from registered_host_identities */
		qsort(hip_reg_table, num_hip_reg, sizeof(hip_reg), 
			compare_hits2);	
		print_reg_table(hip_reg_table);	
	}

	/* Precompute R1s, cookies, DH material */
	init_dh_cache();
	init_all_R1_caches();
	gettimeofday(&time1, NULL);
	last_expire = time1.tv_sec;
#ifdef SMA_CRAWLER
	endbox_init();
	log_(NORM,"Initializing SMA bridge\n");
	struct sockaddr_storage ss_lsi;
	struct sockaddr *lsi = (struct sockaddr*)&ss_lsi;
	lsi->sa_family = AF_INET;
	get_preferred_lsi(lsi);
	char lsi_s[INET_ADDRSTRLEN];
	addr_to_str(SA(lsi), lsi_s, INET_ADDRSTRLEN);
	char cmd[64];
	sprintf(cmd, "/usr/local/etc/hip/bridge_up.sh %s", lsi_s);
	system(cmd);
	last_time = time(NULL);
	ret = getrlimit(RLIMIT_CORE, &limits);
	log_(NORM, "getrlimit returns %d\n", ret);
	log_(NORM, "Current %d hard limit %d\n",
		limits.rlim_cur, limits.rlim_max);
	limits.rlim_cur = limits.rlim_max;
	ret = setrlimit(RLIMIT_CORE, &limits);
	log_(NORM, "setrlimit returns %d\n", ret);
	ret = getrlimit(RLIMIT_CORE, &limits);
	log_(NORM, "getrlimit returns %d\n", ret);
	log_(NORM, "Current %d hard limit %d\n",
		limits.rlim_cur, limits.rlim_max);
	signal(SIGINT, hip_exit);
	signal(SIGTERM, hip_exit);
#else
	signal(SIGINT, hip_exit);
	signal(SIGTERM, hip_exit);
	signal(SIGSEGV, hip_exit);
#endif
	hip_writelock();

	/* Netlink socket */
	if (hip_netlink_open() < 0) {
		log_(ERR, "Netlink socket error: %s\n", strerror(errno));
		goto hip_main_error_exit;
	}
	get_my_addresses();
	select_preferred_address();
	publish_my_hits();
#ifdef __UMH__
#ifndef __WIN32__
	post_init_tap();
#endif
#endif
	/* Status socket */
	if (hip_status_open() < 0) {
		log_(ERR, "Unable to start status socket: %s\n", 
		    strerror(errno));
	}

#ifdef IPV6_HIP
	/* IPv6 HIP socket */
	memset(&addr6, 0, sizeof(addr6));
	addr6.sin6_family = AF_INET6;
	addr6.sin6_port = 0;
	if (str_to_addr((__u8*)"0::0", (struct sockaddr*)&addr6)==0) {
		log_(ERR, "inet_pton() error\n");
		goto hip_main_error_exit;
	}
	s6_hip = socket(PF_INET6, SOCK_RAW, H_PROTO_HIP);
	if (s6_hip < 0) {
		log_(ERR, "raw IPv6 socket() for hipd failed\n");
		goto hip_main_error_exit;
	}
#endif

	/* IPv4 HIP socket */
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	s_hip = socket(PF_INET, SOCK_RAW, H_PROTO_HIP);
	if (s_hip < 0) {
		log_(ERR, "raw IPv4 socket() for hipd failed\n");
		goto hip_main_error_exit;
	}

	addr_udp.sin_family = AF_INET;
	addr_udp.sin_addr.s_addr = htonl(INADDR_ANY);
	addr_udp.sin_port = htons (HIP_UDP_PORT);
	s_hip_udp = socket(PF_INET, SOCK_DGRAM, 0);
	if (s_hip_udp < 0) {
		log_(ERR, "UDP IPv4 socket() for hipd failed\n");
		goto hip_main_error_exit;
	}

	/* PF_KEY socket */
#ifndef __UMH__
	s_pfk = socket(PF_KEY, SOCK_RAW, PF_KEY_V2);
	if (s_pfk < 0) {
		log_(ERR, "PF_KEY socket() failed.\n%s.\n", strerror(errno));
		goto hip_main_error_exit;
	}
#else
	s_pfk = pfkeysp[1]; /* UMH */
#endif

	/* register PF_KEY types with kernel */
	if ((err = sadb_register(SADB_SATYPE_ESP) < 0)) {
		log_(ERR, "Unable to register PFKEY handler w/kernel.\n");
		goto hip_main_error_exit;
	} else {
#ifdef __UMH__
		log_(NORMT, 
		    "Registered PF_KEY handler with usermode thread.\n");
#else
		log_(NORMT, 
		    "Registered PF_KEY handler with the kernel.\n");
#endif
	}

#ifndef __WIN32__
	/* setup message header with control and receive buffers */
	msg.msg_name = &addr_from;
   	msg.msg_namelen = sizeof(struct sockaddr_storage);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
#if !defined(__MACOSX__)
	memset(cbuff, 0, sizeof(cbuff));
	msg.msg_control = cbuff;
	msg.msg_controllen = sizeof(cbuff);
	msg.msg_flags = 0;
#endif
	memset(buff, 0, sizeof(buff));
	iov.iov_len = sizeof(buff);
	iov.iov_base = buff;

#if !defined(__MACOSX__)
	/* indicate that socket wants to receive ICMP messages */
	setsockopt(s_hip, SOL_IP, IP_RECVERR, &optval, sizeof(optval));
	setsockopt(s_hip, IPPROTO_IP, IP_PKTINFO, &optval, sizeof(optval));
#endif
#endif /* __WIN32__ */
	/* bind to specific local address */
	if (bind(s_hip, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		log_(ERR, "bind() for IPv4 HIP socket failed.\n");
		goto hip_main_error_exit;
	}

#if !defined(__WIN32__) && !defined(__MACOSX__)
	setsockopt(s_hip_udp, IPPROTO_IP, IP_PKTINFO, &optval_udp,
			sizeof(optval_udp));
#endif /* __WIN32__ */
#ifndef __MACOSX__
	if (bind(s_hip_udp, SA(&addr_udp), sizeof(addr_udp)) < 0) {
		log_(ERR, "bind() for IPv4 UDP HIP socket failed.\n");
		goto hip_main_error_exit;
	}
#endif

#ifdef IPV6_HIP
	setsockopt(s6_hip, IPPROTO_IPV6, IPV6_RECVERR, &optval, sizeof(optval));
#ifdef IPV6_2292PKTINFO
	setsockopt(s6_hip, IPPROTO_IPV6, IPV6_2292PKTINFO, &optval, sizeof(optval));
#else
	setsockopt(s6_hip, IPPROTO_IPV6, IPV6_PKTINFO, &optval, sizeof(optval));
#endif
	if (bind(s6_hip, (struct sockaddr*)&addr6, sizeof(addr6)) < 0) {
		log_(ERR, "bind() for IPv6 HIP socket failed.\n");
		goto hip_main_error_exit;
	}
	
	highest_descriptor = maxof(6, s_pfk, s_hip, s_hip_udp, s6_hip, s_net, s_stat);
#else /* IPV6_HIP */
	highest_descriptor = maxof(5, s_pfk, s_hip, s_hip_udp, s_net, s_stat);
#endif /* IPV6_HIP */

	log_(NORMT, "Listening on HIP and PF_KEY sockets...\n");

#ifdef HIP_I3
	if (OPT.use_i3)
	        i3_init((hip_hit *)get_preferred_hi(my_hi_head)->hit);
#endif

#ifndef __MACOSX__
  if (OPT.enable_udp) {
	  /* NAT detection using STUN */
	  if (OPT.stun) {
		  log_(NORMT, "STUN: NAT detection with server ");
		  printIPv4Addr (&STUN_server_addr);
		  log_(NORM, "\n");
		  nattype = stunNatType( &STUN_server_addr,FALSE, NULL, NULL, 0, NULL) ;
		  if (nattype == StunTypeOpen || nattype == StunTypeFirewall) {
			  is_behind_nat = FALSE ;
			  log_(NORM, "STUN: No NAT detected.\n");
		  } else {
			  is_behind_nat = TRUE ;
			  log_(NORM, "STUN: NAT detected, UDP encapsulation "
				"activated.\n");
		  }
	  }
	  else { /* Default case when no NAT detection is proceeded, 
            * assume NAT is present */
		  is_behind_nat = TRUE;
		  log_(NORMT, "STUN: No detection proceeded. UDP encapsulation "
				"activated (default).\n");
	  }
  } 
#endif

	/* main event loop */
	for (;;) {
		/* this line causes a performance hit, used for debugging... */
		fflush_log();

#ifdef __UMH__
		if (g_state != 0)
			return(-EINTR);
#endif

		/* prepare file descriptor sets */
		FD_ZERO(&read_fdset);
		FD_SET((unsigned)s_hip, &read_fdset);
		FD_SET((unsigned)s_hip_udp, &read_fdset);
#ifdef IPV6_HIP
		FD_SET((unsigned)s6_hip, &read_fdset);
#endif
		FD_SET((unsigned)s_pfk, &read_fdset);
		FD_SET((unsigned)s_net, &read_fdset);
		FD_SET((unsigned)s_stat, &read_fdset);
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

#ifdef SMA_CRAWLER
                now_time = time(NULL);
                if (now_time - last_time > 60) {
                        log_(NORMT, "hipd_main() heartbeat\n");
                        last_time = now_time;
                        utime("heartbeat_hipd_main", NULL);
                }
#endif

#ifdef HIP_I3
		select_func = OPT.use_i3 ? &cl_select : &select;
		/* wait for I3 socket activity */
		if ((err = (*select_func)((highest_descriptor + 1), &read_fdset,
		    NULL, NULL, &timeout)) < 0) {
#else

		/* wait for socket activity */
		if ((err = select((highest_descriptor + 1), &read_fdset, 
		    NULL, NULL, &timeout)) < 0) {
#endif
			/* sometimes select receives interrupt in addition
			 * to the hip_exit() signal handler */
			if (errno == EINTR)
#ifdef __UMH__
				return(-EINTR);
#else
				hip_exit(SIGINT);
#endif
			log_(WARN, "select() error: %s.\n", strerror(errno));
		} else if (err == 0) { 
			/* idle cycle - select() timeout */
			/* retransmit any waiting packets */
			hip_check_pfkey_buffer();
			hip_retransmit_waiting_packets(&time1);
			hip_handle_state_timeouts(&time1);
			hip_handle_registrations(&time1);
#ifndef __WIN32__	/* cleanup zombie processes from fork() */
			waitpid(0, &err, WNOHANG);
#endif
			/* by default, every 5 minutes */
			if ((time1.tv_sec-last_expire) > (int)HCNF.r1_lifetime){
				last_expire = time1.tv_sec;
				/* expire old DH contexts */
				expire_old_dh_entries();
				/* precompute a new R1 for each HI, and
				 * sometimes pick a new random index for
				 * cookies */
				replace_next_R1();
			}
			if (OPT.trigger) {
				hip_trigger(OPT.trigger);
			}
			if (need_select_preferred) {
				need_select_preferred = FALSE;
				select_preferred_address();
				publish_my_hits();
			}
		} else if (FD_ISSET(s_hip, &read_fdset)) { 
			/* Something on HIP socket */
			flags = 0;
			memset(buff, 0, sizeof(buff));
#ifdef __UMH__
			/* extra check to prevent recvmsg() from blocking */
			if (g_state != 0)
				return(-EINTR);
#endif
#ifdef __WIN32__
			addr_from_len = sizeof(addr_from);
			length = recvfrom(s_hip, buff, sizeof(buff), flags,
					  SA(&addr_from), &addr_from_len);
#else
			length = recvmsg(s_hip, &msg, flags);
#endif
			/* ICMP packet */
			if (length < 0) {
				if (D_VERBOSE == OPT.debug) {
					log_(NORMT, "Received ICMP error ");
					log_(NORM,  "(count=%d) - %d %s\n", 
					    ++num_icmp_errors, 
					    errno, strerror(errno));
		    		}
#ifndef __MACOSX__
#ifndef __WIN32__
		    		/* retrieve ICMP message before looping */ 
		    		flags = MSG_ERRQUEUE;
		    		length = recvmsg(s_hip, &msg, flags);
				/*
				 * Presently, we do not do anything
				 * with ICMP messages
				 */
#endif
#endif
			} else { /* HIP packet */
#ifdef __WIN32__
				hip_handle_packet(buff, length, SA(&addr_from), FALSE);
#else
				hip_handle_packet(&msg, length, AF_INET, FALSE);
#endif
			} 
		} else if (FD_ISSET(s_hip_udp, &read_fdset)) {
			/* Something on HIP-UDP socket */
			flags = 0;
			memset(&buff, 0, sizeof(buff));
#ifdef __WIN32__
			addr_from_len = sizeof(addr_from);
			length = recvfrom(s_hip_udp, buff, sizeof(buff), flags,
					  SA(&addr_from), &addr_from_len);
			if ((length == 1) && (buff[0] == 0xFF)) {
#else
			length = recvmsg(s_hip_udp, &msg, flags);
			if ((length == 1) && (((__u8*)msg.msg_iov->iov_base)[0] == 0xFF)) {
#endif
				/* UDP keep-alive for HIP tunnel */
				printf ("HIP-keepalive received.\n");
			} else { /* HIP packet */
#ifdef __WIN32__
				hip_handle_packet(buff, length, SA(&addr_from), TRUE);
#else
				hip_handle_packet(&msg, length, AF_INET, TRUE);
#endif
			}
#ifdef IPV6_HIP
		} else if (FD_ISSET(s6_hip, &read_fdset)) { 
			/* Something on HIP v6 socket */
			flags = 0;
			memset(&buff, 0, sizeof(buff));
#ifdef __WIN32__
			addr_from_len = sizeof(addr_from);
			length = recvfrom(s6_hip, buff, sizeof(buff), flags,
					  SA(&addr_from), &addr_from_len);
#else
			length = recvmsg(s6_hip, &msg, flags);
#endif
			/* ICMPv6 packet */
			if (length < 0) {
				if (D_VERBOSE == OPT.debug) {
					log_(NORMT, "Received ICMPv6 error ");
					log_(NORM,  "(count=%d) - %d %s\n", 
					    ++num_icmp_errors, 
					    errno, strerror(errno));
				}
			} else {
#ifdef __WIN32__
				hip_handle_packet(buff, length, SA(&addr_from), FALSE);
#else
				hip_handle_packet(&msg, length, AF_INET6, FALSE);
#endif
			}
#endif /* IPV6_HIP */
		} else if (FD_ISSET(s_pfk, &read_fdset)) {
			/* Something on PF_KEY socket */
#ifdef __WIN32__
			if ((length = recv(s_pfk, buff, sizeof(buff), 0)) < 0) {
#else
			if ((length = read(s_pfk, buff, sizeof(buff))) < 0) {
#endif
				log_(WARN, "PFKEY read() error - %d %s\n", 
				    errno, strerror(errno));
			} else {
				hip_handle_pfkey(buff);
			}
		} else if (FD_ISSET(s_net, &read_fdset)) {
			/* Something on Netlink socket */
#ifdef __WIN32__
			if ((length = recv(s_net, buff, sizeof(buff), 0)) < 0) {
#else
			if ((length = read(s_net, buff, sizeof(buff))) < 0) {
#endif
				log_(WARN, "Netlink read() error - %d %s\n", 
				    errno, strerror(errno));
			} else {
				if (hip_handle_netlink(buff, length) == 1) {
					/* changes to address require new 
					 * preferred address */
					need_select_preferred = TRUE;
				}
			}
		} else if (FD_ISSET(s_stat, &read_fdset)) { 
			/* Something on Status socket */
			flags = 0;
			memset(&buff, 0, sizeof(buff));
			addr_from_len = sizeof(addr_from);
			length = sizeof(buff);
			if ((length = recvfrom(s_stat, buff, length, flags,
					SA(&addr_from), &addr_from_len)) < 0) {
#ifdef __WIN32__
				log_(WARN, "Status read() ");
				log_WinError(GetLastError());
#else
				log_(WARN, "Status read() error - %d %s\n",
				    errno, strerror(errno));
#endif
			} else {
				hip_handle_status_request((__u8*)buff, length, 
							  SA(&addr_from));
			}
		} else {
			log_(NORMT, "unknown socket activity.");
		} /* select */
	} /* end for(;;) */
	return(0);
hip_main_error_exit:
#ifndef __WIN32__
	snprintf(buff, sizeof(buff), "%s/run/%s", LOCALSTATEDIR,
		HIP_LOCK_FILENAME);
	unlink(buff);
#endif
	exit(1);
}

/*
 * HIP packet handling routines
 */

/*
 * Check HIP packet for sanity.  Switch on the type of packet and call
 * separate handling routines.
 *
 * buff:  pointer to datagram data (including IP header for IPv4)
 * length:  length of datagram
 */
#ifdef __WIN32__
void hip_handle_packet(__u8* buff, int length, struct sockaddr *src, int use_udp)
{
	__u16 family;
#else
void hip_handle_packet(struct msghdr *msg, int length, __u16 family, int use_udp)
{
	__u8 *buff;
	struct sockaddr *src;
#endif
#ifdef __CYGWIN__
	hi_node *hi;
	sockaddr_list *a;
#else
#ifndef __WIN32__
	struct cmsghdr *cmsg;
#endif
	struct in6_pktinfo *pktinfo = NULL;
#endif
	char typestr[12];
	hiphdr* hiph = NULL;
	hip_assoc* hip_a = NULL;
	hip_hit hit_tmp;
	int err = 0;

	__u16 peer_src_port = 0;
#ifndef __MACOSX__
	struct in_pktinfo *pktinfo_v4 = NULL;
#endif

	struct sockaddr_storage dst_ss;
	struct sockaddr *dst;

#ifndef __WIN32__
	struct sockaddr_storage src_ss;
#ifndef __MACOSX__
	struct sockaddr_in *temp_addr;
	struct sockaddr_in6 *temp_addr6;
#endif

	buff = msg->msg_iov->iov_base;
	src = (struct sockaddr*) &src_ss;
	memset(src, 0, sizeof(struct sockaddr_storage));
#endif
	dst = (struct sockaddr*) &dst_ss;
	memset(dst, 0, sizeof(struct sockaddr_storage));

#ifndef __WIN32__
	/* TODO: need proper ifdefs here for WIN32/IPv6 addresses */
	/* for IPv6, we determine the src/dst addresses here */
	if (family==AF_INET6) {
		/* destination address comes from ancillary data passed
		 * with msg due to IPV6_PKTINFO socket option */
		for (cmsg=CMSG_FIRSTHDR(msg); cmsg; cmsg=CMSG_NXTHDR(msg,cmsg)){
#ifdef IPV6_2292PKTINFO
			if ((cmsg->cmsg_level == IPPROTO_IPV6) && 
			    (cmsg->cmsg_type == IPV6_2292PKTINFO)) {
#else
			if ((cmsg->cmsg_level == IPPROTO_IPV6) && 
			    (cmsg->cmsg_type == IPV6_PKTINFO)) {
#endif
				pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
				break;
			}
		}
		if (!pktinfo) {
			log_(NORMT,"Could not determine IPv6 dst, dropping.\n");
			return;
		}
		dst->sa_family = AF_INET6;
		memcpy(SA2IP(dst), &pktinfo->ipi6_addr, SAIPLEN(dst));
		/* source address is filled in from call
		 * to recvmsg() */
		src->sa_family = AF_INET6;
		memcpy(SA2IP(src), SA2IP(msg->msg_name), SAIPLEN(src));
		
		/* debug */
		log_(NORMT, "IPv6 packet from %s to ", logaddr(src));
		log_(NORM, "%s on if %d.\n",logaddr(dst),pktinfo->ipi6_ifindex);
	}
#else  /* !__WIN32__ */
	family = src->sa_family;
#endif /* !__WIN32__ */

#ifndef __MACOSX__
	if (use_udp) {
#ifdef __WIN32__
		if (family == AF_INET) {
			peer_src_port = ((struct sockaddr_in*)src)->sin_port;
		} else {
			peer_src_port = ((struct sockaddr_in6*)src)->sin6_port;
		}
#else /* __WIN32__ */
		if (family==AF_INET) {
			temp_addr = (struct sockaddr_in *) msg->msg_name ;
			peer_src_port = ntohs(temp_addr->sin_port);
			memcpy (src, temp_addr, sizeof(struct sockaddr_in));
			for (cmsg=CMSG_FIRSTHDR(msg); cmsg; 
			     cmsg=CMSG_NXTHDR(msg,cmsg)) {
				if ((cmsg->cmsg_level == IPPROTO_IP) && 
				    (cmsg->cmsg_type == IP_PKTINFO)) {
					pktinfo_v4 = 
					    (struct in_pktinfo*)CMSG_DATA(cmsg);
					break;
				}
			}
			if (!pktinfo_v4) {
				log_(NORMT, 
				  "Could not determine IPv4 dst, dropping.\n");
				return;
			}
			dst->sa_family = AF_INET;
			memcpy(SA2IP(dst), &pktinfo_v4->ipi_addr, SAIPLEN(dst));
		}
		else {
			temp_addr6 = (struct sockaddr_in6 *) msg->msg_name ;
			peer_src_port = ntohs(temp_addr6->sin6_port);
		}
#endif /* __WIN32__ */
	}
#endif /* __MACOSX__ */

	err = hip_parse_hdr(buff, length, src, dst, family, &hiph, use_udp);

	if (err < 0) {
		/* attempt to send a NOTIFY packet */
		if (VALID_FAM(src) && VALID_FAM(dst) && hiph) {
			hip_a = find_hip_association(src, dst, hiph);
			if ((hip_a) && (hip_a->state >= I1_SENT) &&
			    (hip_a->state < E_FAILED)) {
				log_(WARN, "Header error, sending NOTIFY.\n");
				if (err == -3) /* bad checksum */
					hip_send_notify(hip_a, 
						NOTIFY_CHECKSUM_FAILED, NULL,0);
				else if (err == -2) /* various problems */
					hip_send_notify(hip_a,
						NOTIFY_INVALID_SYNTAX, NULL, 0);
			}
		}
		log_(WARN, "Header error but not enough state for NOTIFY --");
		log_(NORM, "dropping packet.\n");
		return;
	}
	if (hiph == NULL) {
		log_(NORMT, "Dropping HIP packet - bad header\n");
		return; 
	}
	hip_packet_type(hiph->packet_type, typestr);
	log_(NORMT, "Received %s packet from %s", typestr, logaddr(src));
	log_(NORM, " on %s socket length %d\n", use_udp ? "udp" : "raw", length);

	/* lookup using addresses and HITs */
	hip_a = find_hip_association(src, dst, hiph);
	/* for opportunistic HIP, adopt unknown HIT from R1 */
	if ((hip_a == NULL) && OPT.opportunistic && 
	    (hiph->packet_type == HIP_R1)) {
		/* perform lookup with a zero HIT */
		memcpy(&hit_tmp, hiph->hit_sndr, sizeof(hip_hit));
		memcpy(hiph->hit_sndr, &zero_hit, sizeof(hip_hit));
		if ((hip_a = find_hip_association(src, dst, hiph))) {
			memcpy(hip_a->peer_hi->hit, &hit_tmp, sizeof(hip_hit));
			add_peer_hit(hit_tmp, src);
		}
		/* put the HIT back so signature will verify */
		memcpy(hiph->hit_sndr, &hit_tmp, sizeof(hip_hit));
	}

	/* XXX May allow lookup of other packets based solely on HITs
	 *     in the future. Currently, UPDATE and HIP_R1 are accepted
	 *     from anywhere. */
	if (!hip_a && 
	    ((hiph->packet_type == UPDATE) || (hiph->packet_type == HIP_R1)))
		hip_a = find_hip_association2(hiph);

	if (!hip_a && 
	    (hiph->packet_type != HIP_I1) &&
	    (hiph->packet_type != HIP_I2)) {
		log_(NORMT, "Dropping packet type %s -- no state was ",typestr);
		log_(NORM, "found, need to receive an I1 first.\n");
		return;
	}

	switch(hiph->packet_type) {
	case HIP_I1:
		err = hip_handle_I1((__u8 *)hiph, hip_a, src, dst, 
				&peer_src_port, use_udp);
		break;
	case HIP_R1:
		err = hip_handle_R1((__u8 *)hiph, hip_a, src,
				&peer_src_port, use_udp);
		break;
        case HIP_I2:
		err = hip_handle_I2((__u8 *)hiph, hip_a, src, dst, 
				&peer_src_port, use_udp);
		break;
	case HIP_R2:
		err = hip_handle_R2((__u8 *)hiph, hip_a, 
				&peer_src_port, use_udp);
		break;
	case CER:
		err = hip_handle_CER((__u8 *)hiph, hip_a);
		break;
	case BOS: /* BOS was removed starting with base-01 */
		err = hip_handle_BOS((__u8 *)hiph, src);
		break;
	case UPDATE:
		err = hip_handle_update((__u8 *)hiph, hip_a, src, 
				&peer_src_port, use_udp);
		break;
	case NOTIFY:
		err = hip_handle_notify((__u8 *)hiph, hip_a, 
				&peer_src_port, use_udp);
		break;
	case CLOSE:
	case CLOSE_ACK:
		err = hip_handle_close((__u8 *)hiph, hip_a, 
				&peer_src_port, use_udp);
		break;
	default:
		log_(NORMT, "Unknown HIP packet type(%d), dropping\n", 
		    hiph->packet_type);
		break;
    	} /* end switch */
	if (err)
		log_(NORMT, "Error with %s packet from %s\n",
		    typestr, logaddr(src));
	return;
} 

/*
 * Iterate among HIP connections and retransmit packets if needed,
 * or free them if they have reached HCNF.max_retries
 */
void
hip_retransmit_waiting_packets(struct timeval* time1)
{
	int i, offset;
#ifdef DO_EXTRA_DHT_LOOKUPS
	int err;
	struct sockaddr_storage ss_addr_tmp;
	struct sockaddr *addr_tmp = (struct sockaddr*)&ss_addr_tmp;
#endif
	struct sockaddr *src, *dst;
	hip_assoc *hip_a;
	hiphdr *hiph;
	char typestr[12];

	gettimeofday(time1, NULL);
	for (i=0; i < max_hip_assoc; i++) {
		hip_a = &hip_assoc_table[i];
		if ((hip_a->rexmt_cache.len < 1) ||
		    (TDIFF(*time1, hip_a->rexmt_cache.xmit_time) <=
		     (int)HCNF.packet_timeout))
			continue;

		if ((OPT.no_retransmit == FALSE) &&
		    (hip_a->rexmt_cache.retransmits < (int)HCNF.max_retries) && 
		    (hip_a->state != R2_SENT)) {
			src = (struct sockaddr*) &hip_a->hi->addrs.addr;
			dst = (struct sockaddr*) &hip_a->rexmt_cache.dst;
			if ((src->sa_family != dst->sa_family) &&
			   (get_addr_from_list(my_addr_head, 
					       dst->sa_family, src) < 0)) {
				log_(WARN, "Cannot determine source address for"
				    " retransmission to %s.\n", logaddr(dst));
			}
			offset = hip_a->use_udp ? sizeof(udphdr) : 0;
			hiph = (hiphdr*) &hip_a->rexmt_cache.packet[offset];
#ifdef DO_EXTRA_DHT_LOOKUPS
			/* XXX note that this code has proven problematic */
			/* has the address changed? do a DHT lookup */
			if (!hits_equal(hip_a->peer_hi->hit, zero_hit) &&
			    (err = hip_dht_lookup_address(&hip_a->peer_hi->hit, 
				    			addr_tmp, FALSE) >= 0)) {
				if (memcmp(SA2IP(addr_tmp), SA2IP(dst),
								SAIPLEN(dst))) {
					/* switch to new address and
					 * rewrite checksum */
					log_(NORM, "DHT Address for %s changed",
						logaddr(dst));
					log_(NORM, " to %s.\n", 
						logaddr(addr_tmp));
					update_peer_list_address(
							hip_a->peer_hi->hit,
							dst, addr_tmp);
					memcpy(dst, addr_tmp, SALEN(addr_tmp));
					hiph->checksum = 0;
					hiph->checksum = checksum_packet(
						hip_a->rexmt_cache.packet,
						src, dst);
				} else if (err == 0) {
					log_(NORM, "DHT Address unchanged ");
					log_(NORM, "for %s.\n",
						logaddr(addr_tmp));
				}
			}
#endif /* DO_EXTRA_DHT_LOOKUPS */
			hip_packet_type(hiph->packet_type, typestr);
			log_(NORMT, "Retransmitting %s packet to %s ",
			    typestr, logaddr(dst));
			log_(NORM,  "(attempt %d of %d)...\n",
				hip_a->rexmt_cache.retransmits+1, 
				HCNF.max_retries);
			hip_retransmit(hip_a, hip_a->rexmt_cache.packet, 
			    hip_a->rexmt_cache.len, src, dst);
			gettimeofday(&hip_a->rexmt_cache.xmit_time, NULL);
			hip_a->rexmt_cache.retransmits++;
		} else {
		/* move to state E_FAILED for I1_SENT/I2_SENT */
			switch (hip_a->state) {
			case I1_SENT:
			case I2_SENT:
				set_state(hip_a, E_FAILED);
				break;
			default:
				break;
			}
			clear_retransmissions(hip_a);
		}
	}
}


/* Iterate over HIP connections and handle state timeout.
 */
void hip_handle_state_timeouts(struct timeval *time1)
{
	int i, remove_rxmt, do_close, err;
	hip_assoc *hip_a;
	
	gettimeofday(time1, NULL);
	
	for (i = 0; i < max_hip_assoc; i++) {
		do_close = FALSE;
		remove_rxmt = FALSE;
		hip_a = &hip_assoc_table[i];
		switch (hip_a->state) {
		case R2_SENT: /* R2 -> ESTABLISHED */
			if (check_last_used(hip_a, 1, time1) > 0) {
				set_state(hip_a, ESTABLISHED);
				remove_rxmt = TRUE;
				log_(NORMT, "HIP association %d moved ", i);
				log_(NORM,  "from R2_SENT=>ESTABLISHED ");
				log_(NORM,  "due to incoming ESP data.\n");
			/* any packet sent during UAL minutes? */
			} else if (check_last_used(hip_a, 0, time1) > 0) {
				/* data being sent, compare time */
				if (TDIFF(*time1, hip_a->use_time) >
								(int)HCNF.ual)
					do_close = TRUE;
			/* no packet sent or received, check UAL minutes */
			} else if (TDIFF(*time1, hip_a->state_time) > 
								(int)HCNF.ual){
				do_close = TRUE;
			}
			break;
		case CLOSING:
		case CLOSED:
			if (TDIFF(*time1, hip_a->state_time) > 
				(HCNF.ual + (hip_a->state==CLOSED) ? 
				 (int)(2*HCNF.msl) : (int)HCNF.msl)) {
				set_state(hip_a, UNASSOCIATED);
				log_(NORMT, "HIP association %d moved from", i);
				log_(NORM,  " %s=>UNASSOCIATED\n",
				 (hip_a->state==CLOSED)? "CLOSED":"CLOSING");
				/* max_hip_assoc may decrease here, but this
				 * shouldn't ruin this for loop */
				free_hip_assoc(hip_a);
			}
			break;
		case E_FAILED: /* E_FAILED -> UNASSOCIATED */
			if (TDIFF(*time1, hip_a->state_time) > 
			     (int)HCNF.failure_timeout) {
				set_state(hip_a, UNASSOCIATED);
				log_(NORMT, "HIP association %d moved from", i);
				log_(NORM,  " E_FAILED=>UNASSOCIATED\n");
				free_hip_assoc(hip_a);
			}
			break;
		case ESTABLISHED:
			/*
			 * If a pending rekey has been completely ACKed and
			 * a NES has been received, we can finish the rekey.
			 */
			if ((hip_a->rekey) && (hip_a->rekey->acked) &&
			    (hip_a->peer_rekey) &&
			    (hip_a->peer_rekey->new_spi > 0))	{
				hip_finish_rekey(hip_a, TRUE, hip_a->next_use_udp);
				remove_rxmt = TRUE;
			/*
			 * Fail rekey using stored creation time
			 */
			} else if (hip_a->rekey && 
				   TDIFF(*time1, hip_a->rekey->rk_time) >
				   (int)HCNF.failure_timeout) {
				log_hipa_fromto(QOUT, "Rekey failed (timeout)", 
					hip_a, TRUE, TRUE);
				log_(NORMT, "HIP association %d moved from", i);
				log_(NORM,  " %d=>UNASSOCIATED because of "
					    "rekey failure.\n", hip_a->state);
				set_state(hip_a, UNASSOCIATED);
				delete_associations(hip_a, 0, 0);
				free_hip_assoc(hip_a);
				break;
			}
			/*
			 * Check last used time
			 */
			/* don't send SADB_GETs multiple times per second! */
			if (TDIFF(*time1, hip_a->use_time) < 2)
				break;
			err = check_last_used(hip_a, 1, time1);
			err += check_last_used(hip_a, 0, time1);
			/* no use time available, first check state time for UAL
			 * also check the use time because after a rekey, 
			 * bytes=0 and check_last_used() will return 0, but it
			 * is not time to expire yet due to use_time */
			if ((err == 0) &&
			    (TDIFF(*time1,hip_a->state_time) > (int)HCNF.ual)) {
				/* state time has exceeded UAL */
				if (hip_a->use_time.tv_sec == 0)
					do_close = TRUE; /* no bytes ever sent*/
				else if (TDIFF(*time1,hip_a->use_time) > 
						(int)HCNF.ual)
					do_close = TRUE; /* both state time and
							    use time have 
							    exceeded UAL*/
			/* last used time is available, check for UAL */
			} else if ((err == 2)||(err == 1)) {
				if (TDIFF(*time1, hip_a->use_time) > 
								(int)HCNF.ual)
					do_close = TRUE;
			}
			break;
		default:
			break;
		}
		/* move to CLOSING if flagged */
		if (do_close) {
			log_hipa_fromto(QOUT, "Close initiated (timeout)", 
					hip_a, FALSE, TRUE);
			delete_associations(hip_a, 0, 0);
#ifdef __MACOSX__
                        if(hip_a->ipfw_rule > 0) {
                                del_divert_rule(hip_a->ipfw_rule);
                                hip_a->ipfw_rule = 0;
                        }
#endif
			hip_send_close(hip_a, FALSE);
			set_state(hip_a, CLOSING);
		}
		/* clean up rxmt queue if flagged */
		if (remove_rxmt && hip_a->rexmt_cache.packet)
			clear_retransmissions(hip_a);
		/* age peer locators, verify addresses */
		hip_handle_locator_state_timeouts(hip_a, time1);
	}

}

/* Iterate over HIP connections and handle registrations.
 */
void hip_handle_registrations(struct timeval *time1)
{
	int i, do_update = 0;
	hip_assoc *hip_a;
	
	gettimeofday(time1, NULL);
	
	for (i = 0; i < max_hip_assoc; i++) {
		do_update = 0;
		hip_a = &hip_assoc_table[i];
		if (hip_a->state != ESTABLISHED)
			continue;
		if (hip_a->reg_offered) {
			struct reg_info *reg = hip_a->reg_offered->regs;
			while (reg) {
				if (reg->type == REG_RVS)
					continue;
				if (reg->state == REG_REQUESTED) {
					if (TDIFF(*time1, reg->state_time) > 
							(int)HCNF.ual) {
						reg->state = REG_OFFERED;
						do_update = 1;
					}
				}
				if (reg->state == REG_GRANTED) {
					double tmp;
					tmp = YLIFE (reg->granted_lifetime);
					tmp = pow (2, tmp);
					tmp = 0.9*tmp;
					if (TDIFF(*time1, reg->state_time) > 
							(int)tmp) {
						reg->state = REG_OFFERED;
						do_update = 1;
					}
				}
				reg = reg->next;
			}
		}
		if (do_update)
			hip_send_update(hip_a, NULL, NULL, hip_a->use_udp);
	}
}

/* 
 * hip_handle_locator_state_timeouts()
 *
 * Age peer locators, sending address verification when necessary.
 * ACTIVE or UNVERIFIED -> DEPRECATED - locator lifetime expires
 * ACTIVE -> UNVERIFIED - no traffic and local policy mandates
 *                        reachability (TODO)
 */
void hip_handle_locator_state_timeouts(hip_assoc *hip_a, struct timeval *time1)
{
	sockaddr_list *l;
	struct sockaddr *addrcheck;
	__u32 nonce;

	if (!hip_a->peer_hi)
		return;
	if (hip_a->peer_hi->skip_addrcheck)
		return;
	for (l = &hip_a->peer_hi->addrs; l; l = l->next) {
		if (l->lifetime == 0) /* no locator lifetime set */
			continue;
		if (TDIFF(*time1, l->creation_time) < l->lifetime)
			continue;
		/* address has expired */
		addrcheck = SA(&l->addr);
		if (l->status == ACTIVE || 
		    l->status == UNVERIFIED) {
			l->status = DEPRECATED;
			log_(NORMT, "Locator %s has expired after %d seconds," \
				    " performing address check.\n",
				    logaddr(addrcheck), l->lifetime);
		}
		if (hip_a->rekey) /* UPDATE already pending for  */
			continue; /* some other reason 		 */
		/* perform address check */
		hip_a->rekey = malloc(sizeof(struct rekey_info));
		memset(hip_a->rekey, 0, sizeof(struct rekey_info));
		hip_a->rekey->update_id = ++hip_a->hi->update_id;
		hip_a->rekey->acked = FALSE;
		hip_a->rekey->rk_time.tv_sec = time1->tv_sec;
		RAND_bytes((__u8*)&nonce, sizeof(__u32));
		l->nonce = nonce;
		hip_send_update(hip_a, NULL, addrcheck, hip_a->use_udp);
	} /* end for */
}

/*
 * Manually trigger HIP exchange
 */
int hip_trigger(struct sockaddr *dst)
{
	hip_hit *hitp;
	struct sockaddr *src;
	struct sockaddr_storage src_buff;
	hip_assoc* hip_a = NULL;
	hiphdr hiph;
	hi_node *mine = NULL;
	sockaddr_list *a;

	memset(&src_buff, 0, sizeof(struct sockaddr_storage));
	src = (struct sockaddr*)&src_buff;
		
	log_(NORMT, "Manually triggering exchange with %s.\n", logaddr(dst));
	hitp = hit_lookup(dst);
	if ((hitp == NULL) && (!OPT.opportunistic)) {
		log_(NORM, "HIT for ip %s not found, ", logaddr(dst));
		log_(NORM, "unable to send I1. Add HIT to known_host_");
		log_(NORM, "identities or use opportunistic mode.\n");
		return(-1);
	}
	/* Create pseudo-HIP header for lookup */
	if ((mine = get_preferred_hi(my_hi_head)) == NULL) {
		log_(WARN, "No local identities to use.\n");
		return(-1);
	}
	memcpy(hiph.hit_rcvr, mine->hit, sizeof(hip_hit));
	if (hitp == NULL)
		memcpy(hiph.hit_sndr, &zero_hit, sizeof(hip_hit));
	else
		memcpy(hiph.hit_sndr, hitp, sizeof(hip_hit));
	/* here dst is peer */
	hip_a = find_hip_association(dst, src, &hiph);
	if (hip_a && (hip_a->state > UNASSOCIATED)) {
		/* already have a HIP association for this HIT */
		log_(NORM, "HIP association for ip %s ", logaddr(dst));
		log_(NORM, "already exists -- ignoring trigger.\n");
		return(0);
	} else if (!hip_a) {
		/* Create another HIP association */
		hip_a = init_hip_assoc(mine, (const hip_hit*)&hiph.hit_sndr);
		if (!hip_a) {
			log_(WARN, "Unable to create triggered association.\n");
			/* don't remove trigger here and we will retry later */
			return(-1);
		}
	}

	/* fill in addresses */
	for (a = my_addr_head; a; a = a->next) {
		if (a->addr.ss_family != dst->sa_family)
			continue;
		memset(HIPA_SRC(hip_a), 0, sizeof(struct sockaddr_storage));
		memcpy(HIPA_SRC(hip_a), &a->addr,
		    SALEN(&a->addr));
		if (!a->preferred) /* break if preferred address */
			continue;
		log_(NORM, "Using the configured source address of %s.\n",
		    logaddr(HIPA_SRC(hip_a)));
		break;
	}
	make_address_active(&hip_a->hi->addrs);
	memcpy(HIPA_DST(hip_a), dst, SALEN(dst));
	memcpy(&(hip_a->peer_hi->hit), hiph.hit_sndr, sizeof(hip_hit));

	/* Remove the trigger */
	free(OPT.trigger);
	OPT.trigger = NULL;

	/* Send the I1 */
	if (hip_send_I1(hitp, hip_a, -1) > 0) {
		set_state(hip_a, I1_SENT);
	}
	return(0);
}

/*
 * Manually trigger HIP exchange through a rvs
 */
int hip_trigger_rvs(struct sockaddr *rvs, hip_hit *rsp)
{
        struct sockaddr *src;
        struct sockaddr_storage src_buff;
        hip_assoc* hip_a = NULL;
        hiphdr hiph;
        hi_node *mine = NULL;
        sockaddr_list *a;
	
        memset(&src_buff, 0, sizeof(struct sockaddr_storage));
        src = (struct sockaddr*) &src_buff;

	log_(NORMT,	"Manually triggering exchange with rvs: %s to "  
			"communicate with responder: ", logaddr(rvs));
	print_hex(rsp, HIT_SIZE);
	log_(NORM, "\n");

	/* Create pseudo-HIP header for lookup */
	if ((mine = get_preferred_hi(my_hi_head)) == NULL) {
		log_(WARN, "No local identities to use.\n");
		return(-1);
	}
	memcpy(hiph.hit_rcvr, mine->hit, HIT_SIZE);
	memcpy(hiph.hit_sndr, rsp, HIT_SIZE);

	hip_a = find_hip_association2(&hiph); 		// Looks for an existing hip_association between Initiator & Responder
	if (hip_a && (hip_a->state > UNASSOCIATED)) {
		/* already have a HIP association for this HIT */
		log_(NORM, "HIP association for ip %s ", logaddr(rvs));
		log_(NORM, "already exists -- ignoring trigger.\n");
		return(0);
	} else if (!hip_a) {
		/* Create another HIP association */
		hip_a = init_hip_assoc(mine, (const hip_hit*)rsp);
		if (!hip_a) {
			log_(WARN, "Unable to create triggered association.\n");
			/* don't remove trigger here and we will retry later */
			return(-1);
		}
       }
        /* fill in addresses */
        for (a = my_addr_head; a; a = a->next) {
                if (a->addr.ss_family != rvs->sa_family)
                        continue;
                memset(HIPA_SRC(hip_a), 0, sizeof(struct sockaddr_storage));
                memcpy(HIPA_SRC(hip_a), &a->addr, SALEN(&a->addr));
                if (!a->preferred) /* break if preferred address */
                        continue;
                log_(NORM, "Using the configured source address of %s.\n",
                    logaddr(HIPA_SRC(hip_a)));
                break;
        }
        make_address_active(&hip_a->hi->addrs);
        memcpy(HIPA_DST(hip_a), rvs, SALEN(rvs));

        /* Remove the trigger */
        free(OPT.trigger);
        OPT.trigger = NULL;

        /* Send the I1 */
        if (hip_send_I1(rsp, hip_a, -1) > 0) {
                set_state(hip_a, I1_SENT);
        }

        return(0);
}

