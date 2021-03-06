/*
 *  knockd.c
 * 
 *  Copyright (c) 2004-2011 by Judd Vinet <jvinet@zeroflux.org>
 * 
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, 
 *  USA.
 */

#include "config.h"

#if defined(__FreeBSD__) || defined(__APPLE__)
#define BSDISH
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>

#ifdef BSDISH
#include <sys/socket.h>
#include <netinet/in_systm.h>
#endif

#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <net/if.h>

#ifdef BSDISH
#include <bits/time.h>
#endif

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <getopt.h>
#include <syslog.h>
#include <errno.h>

#include <pcap.h>

#include "list.h"

#ifdef BSDISH
#define ETH_T struct ether_header
#define ETH_HAS_IP(eth) (ntohs(eth->ether_type) == ETHERTYPE_IP)
#define IP_T struct ip
#define IP_GET_HL(ip) (ip->hl)
#define IP_GET_PROTO(ip) (ip->ip_p)
#define IP_GET_SADDR(ip) (ip->ip_src.s_addr)
#define IP_GET_DADDR(ip) (ip->ip_dst.s_addr)
#define TCP_T struct tcphdr
#define TCP_GET_SPORT(tcp) (tcp->th_sport)
#define TCP_GET_DPORT(tcp) (tcp->th_sport)
#define TCP_HAS_FLAG(tcp, field, flag) (tcp->th_flags & flag)
#define UDP_T struct udphdr
#define UDP_GET_SPORT(udp) (udp->uh_sport)
#define UDP_GET_DPORT(udp) (udp->uh_sport)
#else
#define ETH_T struct ethhdr
#define ETH_HAS_IP(eth) (ntohs(eth->h_proto) == ETH_P_IP)
#define IP_T struct iphdr
#define IP_GET_HL(ip) (ip->ihl)
#define IP_GET_PROTO(ip) (ip->protocol)
#define IP_GET_SADDR(ip) (ip->saddr)
#define IP_GET_DADDR(ip) (ip->daddr)
#define TCP_T struct tcphdr
#define TCP_GET_SPORT(tcp) (tcp->source)
#define TCP_GET_DPORT(tcp) (tcp->dest)
#define TCP_HAS_FLAG(tcp, field, flag) (tcp->field)
#define UDP_T struct udphdr
#define UDP_GET_SPORT(udp) (udp->source)
#define UDP_GET_DPORT(udp) (udp->dest)
#endif


static char version[] = PACKAGE_VERSION;

#define SEQ_TIMEOUT 25 /* default knock timeout in seconds */
#define CMD_TIMEOUT 10 /* default timeout in seconds between start and stop commands */
#define SEQ_MAX     32 /* maximum number of ports in a knock sequence */

typedef enum _flag_stat {
	DONT_CARE,  /* 0 */
	SET,        /* 1 */
	NOT_SET     /* 2 */
} flag_stat;

/* knock/event tuples */
typedef struct opendoor {
	char name[128];
	unsigned short seqcount;
	unsigned short sequence[SEQ_MAX];
	unsigned short protocol[SEQ_MAX];
	time_t seq_timeout;
	char *start_command;
	time_t cmd_timeout;
	char *stop_command;
	flag_stat flag_fin;
	flag_stat flag_syn;
	flag_stat flag_rst;
	flag_stat flag_psh;
	flag_stat flag_ack;
	flag_stat flag_urg;
	FILE *one_time_sequences_fd;
	char *pcap_filter_exp;
} opendoor_t;
PMList *doors = NULL;

/* we keep one list of knock attempts, one per IP address,
 * and increment the stage as they progress through the sequence.
 */
typedef struct knocker {
	opendoor_t *door;
	short stage;
	char src[16];   /* IP address */
	char *srchost;  /* Hostname */
	time_t seq_start;
} knocker_t;
PMList *attempts = NULL;

/* function prototypes */
void dprint(char *fmt, ...);
void vprint(char *fmt, ...);
void logprint(char *fmt, ...);
void dprint_sequence(opendoor_t *door, char *fmt, ...);
void cleanup(int signum);
void child_exit(int signum);
void reload(int signum);
void ver();
void usage(int exit_code);
char* strtoupper(char *str);
char* trim(char *str);
void runCommand(char *cmd);
int parseconfig(char *configfile);
int parse_port_sequence(char *sequence, opendoor_t *door);
int get_new_one_time_sequence(opendoor_t *door);
long get_next_one_time_sequence(opendoor_t *door);
int disable_used_one_time_sequence(opendoor_t *door);
long get_current_one_time_sequence_position(opendoor_t *door);
void generate_pcap_filter();
size_t realloc_strcat(char **dest, const char *src, size_t size);
void close_door(opendoor_t *door);
char* get_ip(const char* iface, char *buf, int bufsize);
ssize_t parse_cmd(char* dest, size_t size, const char* cmd, const knocker_t* knocker);
int exec_cmd(char* command, char* name);
void sniff(u_char* arg, const struct pcap_pkthdr* hdr, const u_char* packet);

pcap_t *cap = NULL;
FILE *logfd = NULL;
int lltype = -1;
char myip[32];

int  o_usesyslog = 0;
int  o_syslog    = LOG_DAEMON;
int  o_verbose   = 0;
int  o_debug     = 0;
int  o_daemon    = 0;
int  o_lookup    = 0;
char o_int[32]           = "";		/* default (eth0) is set after parseconfig() */
char o_cfg[PATH_MAX]     = "/etc/knockd.conf";
char o_pidfile[PATH_MAX] = "/var/run/knockd.pid";
char o_logfile[PATH_MAX] = "";

int main(int argc, char **argv)
{
	char pcapErr[PCAP_ERRBUF_SIZE] = "";
	int opt, ret, optidx = 1;
        int cap_timeout = 0;
#ifdef BSDISH
	cap_timeout = 1;
#endif
	
	static struct option opts[] =
	{
		{"verbose",   no_argument,       0, 'v'},
		{"debug",     no_argument,       0, 'D'},
		{"daemon",    no_argument,       0, 'd'},
		{"lookup",    no_argument,       0, 'l'},
		{"interface", required_argument, 0, 'i'},
		{"config",    required_argument, 0, 'c'},
		{"help",      no_argument,       0, 'h'},
		{"pidfile",   required_argument, 0, 'p'},
		{"logfile",   required_argument, 0, 'g'},
		{"version",   no_argument,       0, 'V'},
		{0, 0, 0, 0}
	};
	
	if(parseconfig(o_cfg)) {
		usage(1);
	}
	
	while((opt = getopt_long(argc, argv, "vDdli:c:p:g:hV", opts, &optidx))) {
		if(opt < 0) {
			break;
		}
		switch(opt) {
			case 0:   break;
			case 'v': o_verbose = 1; break;
			case 'D': o_debug = 1; break;
			case 'd': o_daemon = 1; break;
			case 'l': o_lookup = 1; break;
			case 'i': strncpy(o_int, optarg, sizeof(o_int)-1);
								o_int[sizeof(o_int)-1] = '\0';
								break;
			case 'c': strncpy(o_cfg, optarg, sizeof(o_cfg)-1);
								o_cfg[sizeof(o_cfg)-1] = '\0';
								break;
			case 'p': strncpy(o_pidfile, optarg, sizeof(o_pidfile)-1);
								o_pidfile[sizeof(o_pidfile)-1] = '\0';
								break;
			case 'g': strncpy(o_logfile, optarg, sizeof(o_logfile)-1);
								o_logfile[sizeof(o_logfile)-1] = '\0';
								break;
			case 'V': ver();
			case 'h': /* fallthrough */
			default: usage(0);
		}
	}

	/* set o_int to a default value if it has not been set by the -i switch nor by
	 * the config file */
	if(strlen(o_int) == 0) {
		strncpy(o_int, "eth0", sizeof(o_int));	/* no explicit termination needed */
	}
	if(o_usesyslog) {
		openlog("knockd", 0, o_syslog);
	}
	if(strlen(o_logfile)) {
		/* open the log file */
		logfd = fopen(o_logfile, "a");
		if(logfd == NULL) {
			perror("warning: cannot open logfile");
		}
	}

	cap = pcap_open_live(o_int, 65535, 0, cap_timeout, pcapErr);
	if(strlen(pcapErr)) {
		fprintf(stderr, "could not open %s: %s\n", o_int, pcapErr);
	}
	if(cap == NULL) {
		exit(1);
	}

	lltype = pcap_datalink(cap);
	switch(lltype) {
		case DLT_EN10MB:
			dprint("ethernet interface detected\n");
			break;
		case DLT_LINUX_SLL:
			dprint("ppp interface detected (linux \"cooked\" encapsulation)\n");
			break;
		case DLT_RAW:
			dprint("raw interface detected, no encapsulation\n");
			break;
		default: 
			fprintf(stderr, "error: unsupported link-layer type: %d\n", lltype);
			cleanup(1);
			break;
	}

	/* get our local IP address */
	if(get_ip(o_int, myip, 32) == NULL) {
		fprintf(stderr, "could not get IP address for %s\n", o_int);
		cleanup(1);
	} else {
		dprint("Local IP: %s\n", myip);
	}

	generate_pcap_filter();

	if(o_daemon) {
		FILE *pidfp;
		if(daemon(0, 0) < 0) {
			perror("daemon");
			cleanup(1);
		}
		/* write our PID to the pidfile*/
		if((pidfp = fopen(o_pidfile, "w"))) {
			fprintf(pidfp, "%d\n", getpid());
			fclose(pidfp);
		} else {
			dprint("could not create pid file %s: %s\n", o_pidfile, strerror(errno));
			logprint("could not create pid file %s: %s", o_pidfile, strerror(errno));
		}
	}

	signal(SIGINT, cleanup);
	signal(SIGTERM, cleanup);
	signal(SIGCHLD, child_exit);
	signal(SIGHUP, reload);

	vprint("listening on %s...\n", o_int);
	logprint("starting up, listening on %s", o_int);
	ret = 1;
	while(ret >= 0) {
		ret = pcap_dispatch(cap, -1, sniff, NULL);
	}
	dprint("bailed out of main loop! (ret=%d)\n", ret);
	pcap_perror(cap, "pcap");

	cleanup(0);
	/* notreached */
	exit(0);
}

void dprint(char *fmt, ...)
{
	va_list args;
	if(o_debug) {
		va_start(args, fmt);
		vprintf(fmt, args);
		va_end(args);
		fflush(stdout);
	}
}

void vprint(char *fmt, ...)
{
	va_list args;
	if(o_verbose) {
		va_start(args, fmt);
		vprintf(fmt, args);
		va_end(args);
		fflush(stdout);
	}
}

/* Output a message to syslog and/or a logfile */
void logprint(char *fmt, ...)
{
	char msg[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(msg, 1024, fmt, args);
	va_end(args);
	if(o_usesyslog) {
		syslog(LOG_NOTICE, "%s", msg);
	}
	if(logfd) {
		time_t t;
		struct tm *tm;
		t = time(NULL);
		tm = localtime(&t);
		
		fprintf(logfd, "[%04d-%02d-%02d %02d:%02d] %s\n", tm->tm_year+1900,
			tm->tm_mon+1, tm->tm_mday, tm->tm_hour, tm->tm_min, msg);
		fflush(logfd);
	}
}

/* Output current sequence of door for debugging */
void dprint_sequence(opendoor_t *door, char *fmt, ...)
{
	va_list args;
	int i;

	if(o_debug) {
		va_start(args, fmt);
		vprintf(fmt, args);
		va_end(args);
		for(i = 0; i < door->seqcount; i++) {
			switch(door->protocol[i]){
				case IPPROTO_UDP:
					printf((i == door->seqcount-1 ? "%u:udp\n" : "%u:udp,"), door->sequence[i]);
					break;
				case IPPROTO_TCP: /* fallthrough */
				default: 
					printf((i == door->seqcount-1 ? "%u:tcp\n" : "%u:tcp,"), door->sequence[i]);
			}
		}
		fflush(stdout);
	}
}

/* Signal handlers */
void cleanup(int signum)
{
	int status;

	vprint("waiting for child processes...\n");
	logprint("waiting for child processes...");
	wait(&status);

	vprint("closing...\n");
	logprint("shutting down");
	pcap_close(cap);
	if(o_daemon) {
		unlink(o_pidfile);
	}
	exit(signum);
}

void child_exit(int signum)
{
	int status;

	while ( waitpid( (pid_t)-1, &status, WNOHANG ) > 0 ) continue;
	return;
}

void reload(int signum)
{
	PMList *lp;
	opendoor_t *door;

	vprint("Re-reading config file: %s\n", o_cfg);
	logprint("Re-reading config file: %s\n", o_cfg);

	for(lp = doors; lp; lp = lp->next) {
		door = (opendoor_t*)lp->data;
		close_door(door);
	}
	list_free(doors);

	parseconfig(o_cfg);

	vprint("Closing and re-opening log file: %s\n", o_logfile);
	logprint("Closing and re-opening log file: %s\n", o_logfile);

	/* close and re-open the log file */
	if(logfd) {
		fclose(logfd);
	}
	logfd = fopen(o_logfile, "a");
	if(logfd == NULL) {
		perror("warning: cannot open logfile");
	}
	
	return;
}

void usage(int exit_code) {
	printf("usage: knockd [options]\n");
	printf("options:\n");
	printf("  -i, --interface <int>  network interface to listen on (default \"eth0\")\n");
	printf("  -d, --daemon           run as a daemon\n");
	printf("  -c, --config <file>    use an alternate config file\n");
	printf("  -D, --debug            output debug messages\n");
	printf("  -l, --lookup           lookup DNS names (may be a security risk)\n");
	printf("  -p, --pidfile          use an alternate pidfile\n");
	printf("  -g, --logfile          use an alternate logfile\n");
	printf("  -v, --verbose          be verbose\n");
	printf("  -V, --version          display version\n");
	printf("  -h, --help             this help\n");
	printf("\n");
	exit(exit_code);
}

void ver() {
	printf("knockd %s\n", version);
	printf("Copyright (C) 2004-2011 Judd Vinet <jvinet@zeroflux.org>\n");
	exit(0);
}

/* Convert a string to uppercase
 */
char* strtoupper(char *str)
{
	char *ptr = str;

	while(*ptr) {
		(*ptr) = toupper(*ptr);
		ptr++;
	}
	return str;
}

/* Trim whitespace and newlines from a string
 */
char* trim(char *str)
{
	char *pch = str;
	while(isspace(*pch)) {
		pch++;
	}
	if(pch != str) {
		memmove(str, pch, (strlen(pch) + 1));
	}
	
	pch = (char*)(str + (strlen(str) - 1));
	while(isspace(*pch)) {
		pch--;
	}
	*++pch = '\0';

	return str;
}

/* Parse a config file
 */
int parseconfig(char *configfile)
{
	FILE *fp = NULL;
	char line[PATH_MAX+1];
	char *ptr = NULL;
	char *key = NULL;
	int linenum = 0;
	char section[256] = "";
	opendoor_t *door = NULL;
	PMList *lp;

	if((fp = fopen(configfile, "r")) == NULL) {
		perror(configfile);
		return(1);
	}

	while(fgets(line, PATH_MAX, fp)) {
		linenum++;
		trim(line);
		if(strlen(line) == 0 || line[0] == '#') {
			continue;
		}
		if(line[0] == '[' && line[strlen(line)-1] == ']') {
			/* new config section */
			ptr = line;
			ptr++;
			strncpy(section, ptr, sizeof(section));
			section[strlen(section)-1] = '\0';
			dprint("config: new section: '%s'\n", section);
			if(!strlen(section)) {
				fprintf(stderr, "config: line %d: bad section name\n", linenum);
				return(1);
			}
			if(strcmp(section, "options")) {
				/* start a new knock/event record */
				door = malloc(sizeof(opendoor_t));
				if(door == NULL) {
					perror("malloc");
					exit(1);
				}
				strncpy(door->name, section, sizeof(door->name)-1);
				door->name[sizeof(door->name)-1] = '\0';
				door->seqcount = 0;
				door->seq_timeout  = SEQ_TIMEOUT; /* default sequence timeout (seconds)  */
				door->start_command = NULL;
				door->cmd_timeout = CMD_TIMEOUT; /* default command timeout (seconds) */
				door->stop_command = NULL;
				door->one_time_sequences_fd = NULL;
				door->pcap_filter_exp = NULL;
				doors = list_add(doors, door);
			}
		} else {
			/* directive */
			if(!strlen(section)) {
				fprintf(stderr, "config: line %d: all directives must belong to a section\n", linenum);
				return(1);
			}
			ptr = line;
			key = strsep(&ptr, "=");
			if(key == NULL) {
				fprintf(stderr, "config: line %d: syntax error\n", linenum);
				return(1);
			}
			trim(key);
			key = strtoupper(key);
			if(ptr == NULL) {
				if(!strcmp(key, "USESYSLOG")) {
					o_usesyslog = 1;
					dprint("config: usesyslog\n");
				} else {
					fprintf(stderr, "config: line %d: syntax error\n", linenum);
					return(1);			
				}
			} else {
				trim(ptr);
				if(!strcmp(section, "options")) {
					if(!strcmp(key, "LOGFILE")) {
						strncpy(o_logfile, ptr, PATH_MAX-1);
						o_logfile[PATH_MAX-1] = '\0';
						dprint("config: log file: %s\n", o_logfile);
					} else if(!strcmp(key, "PIDFILE")) {
						strncpy(o_pidfile, ptr, PATH_MAX-1);
						o_pidfile[PATH_MAX-1] = '\0';
						dprint("config: pid file: %s\n", o_pidfile);
					} else if(!strcmp(key, "INTERFACE")) {
						/* set interface only if it has not already been set by the -i switch */
						if(strlen(o_int) == 0) {
							strncpy(o_int, ptr, sizeof(o_int)-1);
							o_int[sizeof(o_int)-1] = '\0';
							dprint("config: interface: %s\n", o_int);
						}
					} else if(!strcmp(key, "SYSLOGFACILITY")) {
						int i = 0;
						while(1) {
							/* from syslog.h */
							const char *name = facilitynames[i].c_name;
							if(!name) {
								fprintf(stderr, "config: line %d: unknown facility\n", linenum);
								return 1;
							}
							if(!strcmp(name, ptr)) {
								o_syslog = facilitynames[i].c_val;
								dprint("config: syslogfacility: %s (%d)\n", name, o_syslog);
								break;
							}
							i++;
						}
					} else {
						fprintf(stderr, "config: line %d: syntax error\n", linenum);
						return(1);
					}
				} else {
					if(door == NULL) {
						fprintf(stderr, "config: line %d: \"%s\" can only be used within a Door section\n",
								linenum, key);
						return(1);
					}
					if(!strcmp(key, "SEQUENCE")) {
						int i;
						i = parse_port_sequence(ptr, door);
						if (i > 0) {
							return(i);
						}
						dprint_sequence(door, "config: %s: sequence: ", door->name);
					} else if(!strcmp(key, "ONE_TIME_SEQUENCES")) {
						if((door->one_time_sequences_fd = fopen(ptr, "r+")) == NULL) {
							perror(ptr);
							return(1);
						}
						dprint("config: %s: one time sequences file: %s\n", door->name, ptr);
						if (get_new_one_time_sequence(door) == 0) {
							dprint_sequence(door, "config: %s: sequence: ", door->name);
						} else {	/* no more sequences left in the one time sequences file */
							dprint("config: no more sequences left in the one time sequences file %s\n", ptr);
							return(1);
						}
					} else if(!strcmp(key, "SEQ_TIMEOUT") || !strcmp(key, "TIMEOUT")) {
						door->seq_timeout = (time_t)atoi(ptr);
						dprint("config: %s: seq_timeout: %d\n", door->name, door->seq_timeout);
					} else if(!strcmp(key, "START_COMMAND") || !strcmp(key, "COMMAND")) {
						door->start_command = malloc(sizeof(char) * (strlen(ptr)+1));
						if(door->start_command == NULL) {
							perror("malloc");
							exit(1);
						}
						strcpy(door->start_command, ptr);
						dprint("config: %s: start_command: %s\n", door->name, door->start_command);
					} else if(!strcmp(key, "CMD_TIMEOUT")) {
						door->cmd_timeout = (time_t)atoi(ptr);
						dprint("config: %s: cmd_timeout: %d\n", door->name, door->cmd_timeout);
					} else if(!strcmp(key, "STOP_COMMAND")) {
						door->stop_command = malloc(sizeof(char) * (strlen(ptr)+1));
						if(door->stop_command == NULL) {
							perror("malloc");
							exit(1);
						}
						strcpy(door->stop_command, ptr);
						dprint("config: %s: stop_command: %s\n", door->name, door->stop_command);
					} else if(!strcmp(key, "TCPFLAGS")) {
						char *flag;
						strtoupper(ptr);
						while((flag = strsep(&ptr, ","))) {
							/* allow just some flags to be specified */
							if(!strcmp(flag,"FIN")) {
								door->flag_fin = SET;
							} else if(!strcmp(flag,"!FIN")) {
								door->flag_fin = NOT_SET;
							} else if(!strcmp(flag, "SYN")) {
								door->flag_syn = SET;
							} else if(!strcmp(flag, "!SYN")) {
								door->flag_syn = NOT_SET;
							} else if(!strcmp(flag, "RST")) {
								door->flag_rst = SET;
							} else if(!strcmp(flag, "!RST")) {
								door->flag_rst = NOT_SET;
							} else if(!strcmp(flag, "PSH")) {
								door->flag_psh = SET;
							} else if(!strcmp(flag, "!PSH")) {
								door->flag_psh = NOT_SET;
							} else if(!strcmp(flag, "ACK")) {
								door->flag_ack = SET;
							} else if(!strcmp(flag, "!ACK")) {
								door->flag_ack = NOT_SET;
							} else if(!strcmp(flag, "URG")) {
								door->flag_urg = SET;
							} else if(!strcmp(flag, "!URG")) {
								door->flag_urg = NOT_SET;
							} else {
								fprintf(stderr, "config: line %d: unrecognized flag \"%s\"\n",
										linenum, flag);
								return(1);
							}
							dprint("config: tcp flag: %s\n", flag);
						}
					} else {
						fprintf(stderr, "config: line %d: syntax error\n", linenum);
						return(1);
					}
				}
				line[0] = '\0';
			}
		}
	}
	fclose(fp);

	/* sanity checks */
	for(lp = doors; lp; lp = lp->next) {
		door = (opendoor_t*)lp->data;
		if(door->seqcount == 0) {
			fprintf(stderr, "error: section '%s' has an empty knock sequence\n", door->name);
			return(1);
		}
	}

	return(0);
}

/* Parse a port:protocol sequence. Returns a positive integer on error.
 */
int parse_port_sequence(char *sequence, opendoor_t *door)
{
	char *num;
	char *protocol;
	char *port;

	door->seqcount = 0;	/* reset seqcount */
	while((num = strsep(&sequence, ","))) {
		if(door->seqcount >= SEQ_MAX) {
			fprintf(stderr, "config: section %s: too many ports in knock sequence\n", door->name);
			logprint("error: section %s: too many ports in knock sequence\n", door->name);
			return(1);
		}
		port = strsep(&num, ":");
		door->sequence[door->seqcount++] = (unsigned short)atoi(port);
		if((protocol = strsep(&num, ":"))){
			protocol = strtoupper(trim(protocol));
			if(!strcmp(protocol, "TCP")){
				door->protocol[door->seqcount-1] = IPPROTO_TCP;
			} else if(!strcmp(protocol, "UDP")) {
				door->protocol[door->seqcount-1] = IPPROTO_UDP;
			} else {
				fprintf(stderr,"config: section %s: unknown protocol in knock sequence\n", door->name);
				logprint("error: section %s: unknown protocol in knock sequence\n", door->name);
				return(1);
			}
		} else {
			door->protocol[door->seqcount-1] = IPPROTO_TCP; /* default protocol */
		}
	}
	return(0);
}

/* Read a new sequence from the one time sequences file and update the door.
 */
int get_new_one_time_sequence(opendoor_t *door)
{
	rewind(door->one_time_sequences_fd);
	if(get_next_one_time_sequence(door) < 0) {
		/* disable the door by removing it from the doors list if there are no sequences anymore */
		fprintf(stderr, "no more sequences left in the one time sequences file for door %s --> disabling the door\n", door->name);
		logprint("no more sequences left in the one time sequences file for door %s --> disabling the door\n", door->name);
		close_door(door);
		return(1);
	}
	dprint_sequence(door, "new sequence for door %s: ", door->name);
	
	return(0);
}

/* Search from the current position in the one time sequence file for the next
 * valid sequence and insert it into the door structure. Returns the position of
 * the beginning of the found line within the file or a negative value if no
 * valid sequence has been found.
 */
long get_next_one_time_sequence(opendoor_t *door)
{
	char line[PATH_MAX+1];
	int pos;
	
	pos = ftell(door->one_time_sequences_fd);
	while(fgets(line, PATH_MAX, door->one_time_sequences_fd)) {
		trim(line);
		if(strlen(line) == 0 || line[0] == '#') {
			pos = ftell(door->one_time_sequences_fd);
			continue;
		}
		if(parse_port_sequence(line, door) > 0) {
			/* continue searching if parse_port_sequnce returned with an error */
			continue;
		}
		return(pos);
	}
	/* no valid line found */
	return(-1);
}

/* Remove a one time sequence from the corresponding file (after a successful
 * knock attempt)
 */
int disable_used_one_time_sequence(opendoor_t *door)
{
	long pos = get_current_one_time_sequence_position(door);
	if(pos >= 0) {
		if(fseek(door->one_time_sequences_fd, pos, SEEK_SET) < 0) {
			fprintf(stderr, "error while disabling used one time sequence for door %s --> disabling the door\n", door->name);
			logprint("error while disabling used one time sequence for door %s --> disabling the door\n", door->name);
			close_door(door);
			return(1);
		}
		if(fputc('#', door->one_time_sequences_fd) == EOF) {
			fprintf(stderr, "error while disabling used one time sequence for door %s --> disabling the door\n", door->name);
			logprint("error while disabling used one time sequence for door %s --> disabling the door\n", door->name);
			close_door(door);
			return(1);
		}
	}
	return(0);
}

/* Get the position (beginning of line) in the one time sequence file of the
 * current sequence such that we know where to insert a '#' to disable the
 * sequence in the one time sequence file
 */
long get_current_one_time_sequence_position(opendoor_t *door)
{
	opendoor_t pseudo_door;	/* used to compare sequences in the file and the current sequence in door */
	long pos;

	rewind(door->one_time_sequences_fd);
	pseudo_door.one_time_sequences_fd = door->one_time_sequences_fd;
	
	pos = get_next_one_time_sequence(&pseudo_door);
	while(pos >= 0) {
		if(door->seqcount == pseudo_door.seqcount) {
			if((memcmp((void*) door->sequence, (void*) pseudo_door.sequence, door->seqcount) == 0)
					&& (memcmp((void*) door->protocol, (void*) pseudo_door.protocol, door->seqcount) == 0)) {
				return(pos);
			}
		}
		pos = get_next_one_time_sequence(&pseudo_door);
	}
	return(-1);
}

/* Generate and set the filter for pcap. That way only the relevant packets will
 * be forwarded to us (in sniff()). Note that generate_pcap_filter() will first
 * generate a subfilter (=substring of the whole filter string) for each door if
 * door->pcap_filter_exp is NULL. This behaviour can be used for doors with one
 * time sequences, where the subfilter has to be generated after each sequence.
 */
void generate_pcap_filter()
{
	/* NOTE: We're doing string manipulations in a daemon -- use defensive programming! */
		
	PMList *lp;
	opendoor_t *door;
	char *buffer = NULL;			/* temporary buffer to create the individual filter strings */ 
	size_t bufsize = 0;			/* size of buffer */
	char port_str[10];			/* used by snprintf to convert unsigned short --> string */
	short head_set = 0;			/* flag indicating if protocol head is set (i.e. "((tcp dst port") */
	short tcp_present = 0;			/* flag indicating if TCP is used */
	short udp_present = 0;			/* flag indicating if UDP is used */
	unsigned int i;
	short modified_filters = 0;		/* flag indicating if at least one filter has changed --> recompile the filter */
	struct bpf_program bpf_prog;		/* compiled BPF filter program */
	
	/* generate subfilters for each door having a NULL pcap_filter_exp
	 *
	 * Example filter for one single door:
	 * ((tcp dst port 8000 or 8001 or 8002) and tcp[tcpflags] & tcp-syn != 0) or (udp dst port 4000 or 4001)
	 */
	for(lp = doors; lp; lp = lp->next) {
		door = (opendoor_t*)lp->data;

		if(door->pcap_filter_exp != NULL) {
			continue;
		}

		/* if we get here at least one door had a pcap_filter_exp == NULL */
		modified_filters = 1;
		
		head_set = 0;
		tcp_present = 0;
		udp_present = 0;
		
		/* allocate memory for buffer if needed.
		 * The first allocation will be 200 Bytes (should be large enough for common sequences). If there is
		 * not enough space, a call to realloc_strcat() will eventually increase its size. The buffer will be
		 * reused for successive doors */
		if(buffer == NULL) {
			bufsize = 200;
			buffer = (char*)malloc(sizeof(char) * bufsize);
			if(buffer == NULL) {
				perror("malloc");
				cleanup(1);
			}
			buffer[0] = '\0';
		}

		/* generate filter for all TCP ports (i.e. "((tcp dst port 4000 or 4001 or 4002) and tcp[tcpflags] & tcp-syn != 0)" */
		for(i = 0; i < door->seqcount; i++) {
			if(door->protocol[i] == IPPROTO_TCP) {
				if(!head_set) {		/* first TCP port in the sequence */
					bufsize = realloc_strcat(&buffer, "((tcp dst port ", bufsize);
					head_set = 1;
					tcp_present = 1;
				} else {		/* not the first TCP port in the sequence */
					bufsize = realloc_strcat(&buffer, " or ", bufsize);
				}
				snprintf(port_str, sizeof(port_str), "%hu", door->sequence[i]);		/* unsigned short to string */
				bufsize = realloc_strcat(&buffer, port_str, bufsize);			/* append port number */
			}
		}
		if(tcp_present) {
			bufsize = realloc_strcat(&buffer, ")", bufsize);		/* close parentheses of TCP ports */
		}

		/* append the TCP flag filters */
		if(tcp_present) {
			if(door->flag_fin != DONT_CARE) {
				bufsize = realloc_strcat(&buffer, " and tcp[tcpflags] & tcp-fin ", bufsize);
				if(door->flag_fin == SET) {
					bufsize = realloc_strcat(&buffer, "!= 0", bufsize);
				}
				if(door->flag_fin == NOT_SET) {
					bufsize = realloc_strcat(&buffer, "== 0", bufsize);
				}
			}
			if(door->flag_syn != DONT_CARE) {
				bufsize = realloc_strcat(&buffer, " and tcp[tcpflags] & tcp-syn ", bufsize);
				if(door->flag_syn == SET) {
					bufsize = realloc_strcat(&buffer, "!= 0", bufsize);
				}
				if(door->flag_syn == NOT_SET) {
					bufsize = realloc_strcat(&buffer, "== 0", bufsize);
				}
			}
			if(door->flag_rst != DONT_CARE) {
				bufsize = realloc_strcat(&buffer, " and tcp[tcpflags] & tcp-rst ", bufsize);
				if(door->flag_rst == SET) {
					bufsize = realloc_strcat(&buffer, "!= 0", bufsize);
				}
				if(door->flag_rst == NOT_SET) {
					bufsize = realloc_strcat(&buffer, "== 0", bufsize);
				}
			}
			if(door->flag_psh != DONT_CARE) {
				bufsize = realloc_strcat(&buffer, " and tcp[tcpflags] & tcp-psh ", bufsize);
				if(door->flag_psh == SET) {
					bufsize = realloc_strcat(&buffer, "!= 0", bufsize);
				}
				if(door->flag_psh == NOT_SET) {
					bufsize = realloc_strcat(&buffer, "== 0", bufsize);
				}
			}
			if(door->flag_ack != DONT_CARE) {
				bufsize = realloc_strcat(&buffer, " and tcp[tcpflags] & tcp-ack ", bufsize);
				if(door->flag_ack == SET) {
					bufsize = realloc_strcat(&buffer, "!= 0", bufsize);
				}
				if(door->flag_ack == NOT_SET) {
					bufsize = realloc_strcat(&buffer, "== 0", bufsize);
				}
			}
			if(door->flag_urg != DONT_CARE) {
				bufsize = realloc_strcat(&buffer, " and tcp[tcpflags] & tcp-urg ", bufsize);
				if(door->flag_urg == SET) {
					bufsize = realloc_strcat(&buffer, "!= 0", bufsize);
				}
				if(door->flag_urg == NOT_SET) {
					bufsize = realloc_strcat(&buffer, "== 0", bufsize);
				}
			}
			bufsize = realloc_strcat(&buffer, ")", bufsize);		/* close parentheses of flags */
		}

		/* append filter for all UDP ports (i.e. "(udp dst port 6543 or 6544 or 6545)" */
		head_set = 0;
		for(i = 0; i < door->seqcount; i++) {
			if(door->protocol[i] == IPPROTO_UDP) {
				if(!head_set) {		/* first UDP port in the sequence */
					if(tcp_present) {
						bufsize = realloc_strcat(&buffer, " or ", bufsize);
					}
					bufsize = realloc_strcat(&buffer, "(udp dst port ", bufsize);
					head_set = 1;
					udp_present = 1;
				} else {		/* not the first UDP port in the sequence */
					bufsize = realloc_strcat(&buffer, " or ", bufsize);
				}
				snprintf(port_str, sizeof(port_str), "%hu", door->sequence[i]);		/* unsigned short to string */
				bufsize = realloc_strcat(&buffer, port_str, bufsize);			/* append port number */
			}
		}
		if(udp_present) {
			bufsize = realloc_strcat(&buffer, ")", bufsize);		/* close parentheses of UDP ports */
		}
		
		/* test if in any of the precedent calls to realloc_strcat() failed. We can do this safely here because
		 * realloc_strcat() returns 0 on failure and if a buffer size of 0 is passed to it, the function does
		 * nothing but returning 0 again. Because we never read buffer in the above code, it is secure to test
		 * for failure only at this point (it makes the code more readable than checking for failure each time
		 * realloc_strcat() is called). */
		if(bufsize == 0) {
			perror("realloc");
			cleanup(1);
		}

		/* allocate the buffer in door holding the filter string, copy it and prepare buffer for being reused. */
		door->pcap_filter_exp = (char*)malloc(strlen(buffer) + 1);
		if(door->pcap_filter_exp == NULL) {
			perror("malloc");
			cleanup(1);
		}
		strcpy(door->pcap_filter_exp, buffer);	

		buffer[0] = '\0';	/* "clear" the buffer */
	}


	/* generate the whole pcap filter string if a filter had been modified. Reuse buffer (already "cleared").
	 *
	 * Note that we don't check if a port is included in multiple doors, we simply concatenate the individual door
	 * filters and rely on pcap's optimization capabilities.
	 *
	 * Example filter for two doors with sequences 8000:tcp,4000:udp,8001:tcp,4001:udp,8002:tcp (syn) and 
	 * 1234:tcp,4567:tcp,8901:tcp (syn,ack) :
	 * dst host the.hosts.ip.address and (
	 *      ((tcp dst port 8000 or 8001 or 8002) and tcp[tcpflags] & tcp-syn != 0) or (udp dst port 4000 or 4001)
	 *   or ((tcp dst port 1234 or 4567 or 8901) and tcp[tcpflags] & tcp-syn != 0 and tcp[tcpflags] & tcp-ack != 0)
	 * )
	 */
	if(modified_filters) {
		bufsize = realloc_strcat(&buffer, "dst host ", bufsize);	/* accept only incoming packets */
		bufsize = realloc_strcat(&buffer, myip, bufsize);
		bufsize = realloc_strcat(&buffer, " and (", bufsize);
		/* iterate over all doors */
		for(lp = doors; lp; lp = lp->next) {
			door = (opendoor_t*)lp->data;
			bufsize = realloc_strcat(&buffer, door->pcap_filter_exp, bufsize);
			if(lp->next != NULL) {
				bufsize = realloc_strcat(&buffer, " or ", bufsize);
			}
		}
		bufsize = realloc_strcat(&buffer, ")", bufsize);		/* close parantheses around port filters */

		/* test if in any of the precedent calls to realloc_strcat() failed. See above why this is ok to do this only
		 * at this point */
		if(bufsize == 0) {
			perror("realloc");
			cleanup(1);
		}

		if(pcap_compile(cap, &bpf_prog, buffer, 1, 0) < 0) {	/* optimize filter (1), no netmask (0) (we're not interested in broadcasts) */
			pcap_perror(cap, "pcap");
			cleanup(1);
		}
		if(pcap_setfilter(cap, &bpf_prog) < 0) {
			pcap_perror(cap, "pcap");
			cleanup(1);
		}
		pcap_freecode(&bpf_prog);
		free(buffer);
	}
}

/* Reallocating strcat -- appends the src string to the dest string (pointer to
 * char*!) overwriting the `\0' character at the end of dest, and then adds a
 * terminating `\0' character. size is the whole size of the dest buffer (not
 * the remaining space like in strncat). If there is not enough space to append
 * src, the dest buffer will be realloc()ated to hold the src string. The
 * reallocation is done by doubling dest's size until it is large enough. dest
 * will always be NULL terminated. Returns the size of the whole buffer or 0 if
 * realloc() fails.
 *
 * IMPORTANT: dest has to be an allocated buffer (not static!), such that it can
 * be reallocated with realloc() !!!
 */
size_t realloc_strcat(char **dest, const char *src, size_t size)
{
	size_t needed_size;
	size_t new_size;

	if(size == 0) {
		return 0;
	}

	needed_size = strlen(*dest) + strlen(src) + 1;		/* '+ 1' for '\0' */
	new_size = size;
	
	while(needed_size > new_size) {
		new_size *= 2;
	}
	if(new_size != size) {
		*dest = (char*)realloc(*dest, new_size);
		if(*dest == NULL) {
			return 0;
		}
	}

	/* now dest is large enough to strcat() the src */
	strcat(*dest, src);
	
	return new_size;
}

/* Disable the door by removing it from the doors list and free all allocated memory.
 */
void close_door(opendoor_t *door)
{
	doors = list_remove(doors, door);
	if (door) {
		if (door->start_command) {
			free(door->start_command);
		}
		if (door->stop_command) {
			free(door->stop_command);
		}
		if (door->one_time_sequences_fd) {
			fclose(door->one_time_sequences_fd);
		}
		if (door->pcap_filter_exp) {
			free(door->pcap_filter_exp);
		}
		free(door);
	}
}

/* Get the IP address of an interface
 */
char* get_ip(const char* iface, char *buf, int bufsize)
{
	int s;
	struct ifreq ifr;

	if(bufsize <= 0) {
		return(NULL);
	}
	if(buf == NULL) {
		return(NULL);
	}
	buf[0] = '\0';

#ifdef BSDISH
	s = socket(AF_INET, SOCK_DGRAM, 0);
#else
	s = socket(PF_PACKET, SOCK_DGRAM, 0);
#endif
	if(s < 0) {
		return(NULL);
	}

	bzero((void*)(&ifr.ifr_name), sizeof(ifr.ifr_name));
	strncpy(ifr.ifr_name, iface, sizeof(ifr.ifr_name)-1);
	ifr.ifr_name[sizeof(ifr.ifr_name)-1] = '\0';
	if(ioctl(s, SIOCGIFADDR, &ifr)) {
		close(s);
		return(NULL);
	}
	close(s);

	strncpy(buf, inet_ntoa((*(struct sockaddr_in *)&ifr.ifr_addr).sin_addr), bufsize-1);
	buf[bufsize-1] = '\0';
	return(buf);
}

/* Parse a command line, replacing tokens (eg, %IP%) with their real values and
 * copy the result to dest. At most size-1 characters will be copied and the
 * result will always be NULL terminated (except if size == 0). The return value
 * is the length of the resulting string. If the returned size is larger than the
 * size of dest, then the result has ben truncated. If the command contains an
 * unknown or broken token, -1 is returned.
 */
ssize_t parse_cmd(char* dest, size_t size, const char* cmd, const knocker_t* knocker) {
	size_t ip_len = 0;
	const char *tok = NULL;
	size_t total_len = 0;
	
	/* make sure this string is always zero-teminated, even if we fill it
	 * completely */
	if(size) {
		dest[size - 1] = '\0';
		size--;
	}
	
	while(*cmd != '\0') {
		/* a percent sign must be either the start or end of token */
		if(*cmd == '%') {
			/* store pointer to token if we aren't handling one already */
			if(!tok) {
				tok = cmd + 1;
			}
			else {
				size_t tok_len = cmd - tok - 1;
				/* a zero token is an escaped percent sign */
				if(tok_len == 0) {
					if (size) {
						*dest++ = '%';
						size--;
					}
					total_len++;
				}
				/* paste the IP address if this is the IP token */
				else if(memcmp(tok, "IP", tok_len) == 0) {
					const char *ip = knocker->src;
					if(!ip_len && ip) {
						ip_len = strlen(ip);
					}
					if(size >= ip_len) {
						memcpy(dest, ip, ip_len);
						dest += ip_len;
						size -= ip_len;
					}
					total_len += ip_len;
				}
				/* paste the current system timestamp if this is the TIME token */
				else if(memcmp(tok, "TIME", tok_len) == 0) {
					/* 11 chars are enough for an unsigned int which should last until
					 * year 2100 or so (time_t is probably 64 bit) */
					char now[11];
					int now_len;
					now_len = sprintf(now, "%u", (unsigned)knocker->seq_start);
					if(size >= now_len) {
						memcpy(dest, now, now_len);
						dest += now_len;
						size -= now_len;
					}
					total_len += now_len;
				}
				/* paste the door's name if this is the NAME token */
				else if(memcmp(tok, "NAME", tok_len) == 0) {
					const char *name = knocker->door->name;
					int name_len = strlen(name);
					if(size >= name_len) {
						memcpy(dest, name, name_len);
						dest += name_len;
						size -= name_len;
					}
					total_len += name_len;
				}
				/* paste the hostname, if available and this is the HOST token */
				else if(memcmp(tok, "HOST", tok_len) == 0) {
					const char *host = knocker->srchost;
					int host_len = 0;
					if(host) {
						host_len = strlen(host);
					}
					if(size >= host_len) {
						memcpy(dest, host, host_len);
						dest += host_len;
						size -= host_len;
					}
					total_len += host_len;
				}
				/* this must be an unknown token, bail out (will keep the) token 
				 * pointer so the check below will make us return -1 */
				else {
					break;
				}
				/* look for next token */
				tok = NULL;
			}
		}
		/* copy character if we aren't in a token currently */
		else if(!tok) {
			if(size) {
				*dest++ = *cmd;
				size--;
			}
			total_len++;
		}
		cmd++;
	}
	
	/* zero-terminate this string */
	if(size) {
		*dest = '\0';
	}

	/* we didn't find a closing percent sign or bailed out */
	if(tok) {
		return -1;
	}
	
	return total_len;
}


/* Execute a command through the system shell and wait for return
 */
int exec_cmd(char* command, char* name){
	int ret;

	logprint("%s: running command: %s\n", name, command);
	vprint("%s: running command: %s\n", name, command);
	ret = system(command);
	if(ret == -1) {
		fprintf(stderr, "error: command fork failed!\n");
		logprint("error: command fork failed!");
	} else if(ret != 0) {
		fprintf(stderr, "%s: command returned non-zero status code (%d)\n", name, WEXITSTATUS(ret));
		logprint("%s: command returned non-zero status code (%d)", name, WEXITSTATUS(ret));
	}
	return ret;
}


/* Sniff an interface, looking for port-knock sequences
 */
void sniff(u_char* arg, const struct pcap_pkthdr* hdr, const u_char* packet)
{
	/* packet structs */
	ETH_T* eth = NULL;
	IP_T* ip   = NULL;
	TCP_T* tcp = NULL;
	UDP_T* udp = NULL;
	char proto[8];
	/* TCP/IP data */	
	struct in_addr inaddr;
	unsigned short sport, dport;
	char srcIP[16], dstIP[16];
	/* timestamp */
	time_t pkt_secs = hdr->ts.tv_sec;
	struct tm* pkt_tm;
	char pkt_date[11];
	char pkt_time[9];
	PMList *lp;
	knocker_t *attempt = NULL;
	
	if(lltype == DLT_EN10MB) {
		eth = (ETH_T*)packet;
		if(!ETH_HAS_IP(eth)) {
			return;
		}
		
		ip = (IP_T*)(packet + sizeof(ETH_T));
	} else if(lltype == DLT_LINUX_SLL) {
		ip = (IP_T*)((u_char*)packet + 16);
	} else if(lltype == DLT_RAW) {
		ip = (IP_T*)((u_char*)packet);
	}

	if(ip->version != 4) {	
		/* no IPv6 yet */
		dprint("packet is not %s, ignoring...\n", "IPv4");
		return;
	}
	
	if(IP_GET_PROTO(ip) == IPPROTO_ICMP) {
		/* we don't do ICMP */
		return;
	}

	/* make sure this packet was sent TO us, not FROM us or THROUGH us.
	 * Actually the pcap filter will take care of forwarding only packets
	 * destined for us, but another check won't hurt... */
	if(inet_aton(myip, &inaddr) == 0) {
		fprintf(stderr, "error: could not understand IP address: %s\n", myip);
		return;
	}

	if(IP_GET_DADDR(ip) != inaddr.s_addr) {
		dprint("packet destined for another host, ignoring...\n");
		return;
	}
	
	sport = dport = 0;
	if(IP_GET_PROTO(ip) == IPPROTO_TCP) {
		strncpy(proto, "tcp", sizeof(proto));
		tcp = (TCP_T*)((u_char*)ip + IP_GET_HL(ip) * 4);
		sport = ntohs(TCP_GET_SPORT(tcp));
		dport = ntohs(TCP_GET_DPORT(tcp));
	}
	if(IP_GET_PROTO(ip) == IPPROTO_UDP) {
		strncpy(proto, "udp", sizeof(proto));
		udp = (UDP_T*)((u_char*)ip + IP_GET_HL(ip) * 4);
		sport = ntohs(UDP_GET_SPORT(udp));
		dport = ntohs(UDP_GET_DPORT(udp));
	}

	/* get the date/time */
	pkt_tm = localtime(&pkt_secs);
	snprintf(pkt_date, 11, "%04d-%02d-%02d", pkt_tm->tm_year+1900, pkt_tm->tm_mon,
			pkt_tm->tm_mday);
	snprintf(pkt_time, 9, "%02d:%02d:%02d", pkt_tm->tm_hour, pkt_tm->tm_min,
			pkt_tm->tm_sec);

	/* convert IPs from binary to string */
	inaddr.s_addr = IP_GET_SADDR(ip);
	strncpy(srcIP, inet_ntoa(inaddr), sizeof(srcIP)-1);
	srcIP[sizeof(srcIP)-1] = '\0';
	inaddr.s_addr = IP_GET_DADDR(ip);
	strncpy(dstIP, inet_ntoa(inaddr), sizeof(dstIP)-1);
	dstIP[sizeof(dstIP)-1] = '\0';

	dprint("%s %s: %s: %s:%d -> %s:%d %d bytes\n", pkt_date, pkt_time,
			proto, srcIP, sport, dstIP, dport, hdr->len);

	/* clean up expired/completed/failed attempts */
	for(lp = attempts; lp; lp = lp->next) {
		int nix = 0;
		attempt = (knocker_t*)lp->data;
		if(attempt->stage >= attempt->door->seqcount) {
			dprint("removing successful knock attempt (%s)\n", attempt->src);
			nix = 1;
		}
		if(attempt->stage < 0) {
			dprint("removing failed knock attempt (%s)\n", attempt->src);
			nix = 1;
		}
		if(!nix && (pkt_secs - attempt->seq_start) >= attempt->door->seq_timeout) {
			if(attempt->srchost) {
				vprint("%s (%s): %s: sequence timeout (stage %d)\n", attempt->src, attempt->srchost,
						attempt->door->name, attempt->stage);
				logprint("%s (%s): %s: sequence timeout (stage %d)\n", attempt->src, attempt->srchost,
						attempt->door->name, attempt->stage);
			} else {
				vprint("%s: %s: sequence timeout (stage %d)\n", attempt->src,
						attempt->door->name, attempt->stage);
				logprint("%s: %s: sequence timeout (stage %d)\n", attempt->src,
						attempt->door->name, attempt->stage);
			}
			nix = 1;
		}
		if(nix) {
			knocker_t *k = (knocker_t*)lp->data;
			/* splice this entry out of the list */
			if(lp->prev) lp->prev->next = lp->next;
			if(lp->next) lp->next->prev = lp->prev;
			if(lp == attempts) attempts = NULL;
			lp->prev = lp->next = NULL;
			free(k->srchost);
			list_free(lp);
			continue;
		}
	}

	attempt = NULL;
	/* look for this guy in our attempts list */
	for(lp = attempts; lp; lp = lp->next) {
		if(!strncmp(((knocker_t*)lp->data)->src, srcIP, sizeof(srcIP))) {
			attempt = (knocker_t*)lp->data;
			break;
		}
	}

	if(attempt) {
		int flagsmatch = 1;
		/* if tcp, check the flags to ignore the packets we don't want
		 * (don't even use it to cancel sequences)
		 */
		if(IP_GET_PROTO(ip) == IPPROTO_TCP) {
			if(attempt->door->flag_fin != DONT_CARE) {
				if(attempt->door->flag_fin == SET && !TCP_HAS_FLAG(tcp, fin, TH_FIN)) {
					dprint("packet is not %s, ignoring...\n", "FIN");
					flagsmatch = 0;
				}
				if(attempt->door->flag_fin == NOT_SET && TCP_HAS_FLAG(tcp, fin, TH_FIN)) {
					dprint("packet is not %s, ignoring...\n", "!FIN");
					flagsmatch = 0;
				}
			}
			if(attempt->door->flag_syn != DONT_CARE) {
				if(attempt->door->flag_syn == SET && !TCP_HAS_FLAG(tcp, syn, TH_SYN)) {
					dprint("packet is not %s, ignoring...\n", "SYN");
					flagsmatch = 0;
				}
				if(attempt->door->flag_syn == NOT_SET && TCP_HAS_FLAG(tcp, syn, TH_SYN)) {
					dprint("packet is not %s, ignoring...\n", "!SYN");
					flagsmatch = 0;
				}
			}
			if(attempt->door->flag_rst != DONT_CARE) {
				if(attempt->door->flag_rst == SET && !TCP_HAS_FLAG(tcp, rst, TH_RST)) {
					dprint("packet is not %s, ignoring...\n", "RST");
					flagsmatch = 0;
				}
				if(attempt->door->flag_rst == NOT_SET && TCP_HAS_FLAG(tcp, rst, TH_RST)) {
					dprint("packet is not %s, ignoring...\n", "!RST");
					flagsmatch = 0;
				}
			}
			if(attempt->door->flag_psh != DONT_CARE) {
				if(attempt->door->flag_psh == SET && !TCP_HAS_FLAG(tcp, psh, TH_PUSH)) {
					dprint("packet is not %s, ignoring...\n", "PSH");
					flagsmatch = 0;
				}
				if(attempt->door->flag_psh == NOT_SET && TCP_HAS_FLAG(tcp, psh, TH_PUSH)) {
					dprint("packet is not %s, ignoring...\n", "!PSH");
					flagsmatch = 0;
				}
			}
			if(attempt->door->flag_ack != DONT_CARE) {
				if(attempt->door->flag_ack == SET && !TCP_HAS_FLAG(tcp, ack, TH_ACK)) {
					dprint("packet is not %s, ignoring...\n", "ACK");
					flagsmatch = 0;
				}
				if(attempt->door->flag_ack == NOT_SET && TCP_HAS_FLAG(tcp, ack, TH_ACK)) {
					dprint("packet is not %s, ignoring...\n", "!ACK");
					flagsmatch = 0;
				}
			}
			if(attempt->door->flag_urg != DONT_CARE) {
				if(attempt->door->flag_urg == SET && !TCP_HAS_FLAG(tcp, urg, TH_URG)) {
					dprint("packet is not %s, ignoring...\n", "URG");
					flagsmatch = 0;
				}
				if(attempt->door->flag_urg == NOT_SET && TCP_HAS_FLAG(tcp, urg, TH_URG)) {
					dprint("packet is not %s, ignoring...\n", "!URG");
					flagsmatch = 0;
				}
			}
		}
		if(flagsmatch && IP_GET_PROTO(ip) == attempt->door->protocol[attempt->stage] &&
				dport == attempt->door->sequence[attempt->stage]) {
			/* level up! */
			attempt->stage++;
			if(attempt->srchost) {
				vprint("%s (%s): %s: Stage %d\n", attempt->src, attempt->srchost, attempt->door->name, attempt->stage);
				logprint("%s (%s): %s: Stage %d", attempt->src, attempt->srchost, attempt->door->name, attempt->stage);
			} else {
				vprint("%s: %s: Stage %d\n", attempt->src, attempt->door->name, attempt->stage);
				logprint("%s: %s: Stage %d", attempt->src, attempt->door->name, attempt->stage);
			}
			if(attempt->stage >= attempt->door->seqcount) {
				if(attempt->srchost) {
					vprint("%s (%s): %s: OPEN SESAME\n", attempt->src, attempt->srchost, attempt->door->name);
					logprint("%s (%s): %s: OPEN SESAME", attempt->src, attempt->srchost, attempt->door->name);
				} else {
					vprint("%s: %s: OPEN SESAME\n", attempt->src, attempt->door->name);
					logprint("%s: %s: OPEN SESAME", attempt->src, attempt->door->name);
				}
				if(attempt->door->start_command && strlen(attempt->door->start_command)) {
					/* run the associated command */
					if(fork() == 0) {
						/* child */
						char parsed_start_cmd[PATH_MAX];
						char parsed_stop_cmd[PATH_MAX];
						ssize_t cmd_len = 0;

						setsid();
						
						/* parse start and stop command and check if the parsed commands fit in the given buffer. Don't
						 * execute any command if one of them has been truncated */
						cmd_len = parse_cmd(parsed_start_cmd, sizeof(parsed_start_cmd), attempt->door->start_command, attempt);
						if(cmd_len < 0) {
							fprintf(stderr, "error: start command is malformed! --> won't execute it\n");
							logprint("error: start command is malformed! --> won't execute it");
							exit(0); /* exit child */

						}
						if(cmd_len >= sizeof(parsed_start_cmd)) {	/* command has been truncated --> do NOT execute it */
							fprintf(stderr, "error: parsed start command has been truncated! --> won't execute it\n");
							logprint("error: parsed start command has been truncated! --> won't execute it");
							exit(0); /* exit child */
						}
						if(attempt->door->stop_command) {
							cmd_len = parse_cmd(parsed_stop_cmd, sizeof(parsed_stop_cmd), attempt->door->stop_command, attempt);
							if(cmd_len < 0) {
								fprintf(stderr, "error: stop command is malformed! --> won't execute it\n");
								logprint("error: stop command is malformed! --> won't execute it");
								exit(0); /* exit child */
							}
							if(cmd_len >= sizeof(parsed_stop_cmd)) {	/* command has been truncated --> do NOT execute it */
								fprintf(stderr, "error: parsed stop command has been truncated! --> won't execute start command\n");
								logprint("error: parsed stop command has been truncated! --> won't execute start command");
								exit(0); /* exit child */
							}
						}

						/* all parsing ok --> execute the parsed (%IP% = source IP) command */
						exec_cmd(parsed_start_cmd, attempt->door->name);
						/* if stop_command is set, sleep for cmd_timeout and run it*/
						if(attempt->door->stop_command){
							sleep(attempt->door->cmd_timeout);
							if(attempt->srchost) {
								vprint("%s (%s): %s: command timeout\n", attempt->src, attempt->srchost, attempt->door->name);
								logprint("%s (%s): %s: command timeout", attempt->src, attempt->srchost, attempt->door->name);
							} else {
								vprint("%s: %s: command timeout\n", attempt->src, attempt->door->name);
								logprint("%s: %s: command timeout", attempt->src, attempt->door->name);
							}
							exec_cmd(parsed_stop_cmd, attempt->door->name);
						}
						
						exit(0); /* exit child */
					}
				}
				/* change to next sequence if one time sequences are used.
				 * Note that here the door will eventually be closed in
				 * get_new_one_time_sequence() if no more sequences are left */
				if(attempt->door->one_time_sequences_fd) {
					disable_used_one_time_sequence(attempt->door);
					get_new_one_time_sequence(attempt->door);

					/* update pcap filter */
					free(attempt->door->pcap_filter_exp);
					attempt->door->pcap_filter_exp = NULL;
					generate_pcap_filter();
				}
			}
		} else if(flagsmatch == 0) {
			/* TCP flags didn't match -- just ignore this packet, don't
			 * invalidate the knock.
			 */
		} else {
			/* invalidate the knock sequence, it will be removed in the
			 * next sniff() call.
			 */
			attempt->stage = -1;
		}
	} else {
		/* did they hit the first port correctly? */
		for(lp = doors; lp; lp = lp->next) {
			opendoor_t *door = (opendoor_t*)lp->data;
			/* if we're working with TCP, try to match the flags */
			if(IP_GET_PROTO(ip) == IPPROTO_TCP){
				if(door->flag_fin != DONT_CARE) {
					if(door->flag_fin == SET && !TCP_HAS_FLAG(tcp, fin, TH_FIN)) {
						dprint("packet is not %s, ignoring...\n", "FIN");
						continue;
					}
					if(door->flag_fin == NOT_SET && TCP_HAS_FLAG(tcp, fin, TH_FIN)) {
						dprint("packet is not %s, ignoring...\n", "!FIN");
						continue;
					}
				}
				if(door->flag_syn != DONT_CARE) {
					if(door->flag_syn == SET && !TCP_HAS_FLAG(tcp, syn, TH_SYN)) {
						dprint("packet is not %s, ignoring...\n", "SYN");
						continue;
					}
					if(door->flag_syn == NOT_SET && TCP_HAS_FLAG(tcp, syn, TH_SYN)) {
						dprint("packet is not %s, ignoring...\n", "!SYN");
						continue;
					}
				}
				if(door->flag_rst != DONT_CARE) {
					if(door->flag_rst == SET && !TCP_HAS_FLAG(tcp, rst, TH_RST)) {
						dprint("packet is not %s, ignoring...\n", "RST");
						continue;
					}
					if(door->flag_rst == NOT_SET && TCP_HAS_FLAG(tcp, rst, TH_RST)) {
						dprint("packet is not %s, ignoring...\n", "!RST");
						continue;
					}
				}
				if(door->flag_psh != DONT_CARE) {
					if(door->flag_psh == SET && !TCP_HAS_FLAG(tcp, psh, TH_PUSH)) {
						dprint("packet is not %s, ignoring...\n", "PSH");
						continue;
					}
					if(door->flag_psh == NOT_SET && TCP_HAS_FLAG(tcp, psh, TH_PUSH)) {
						dprint("packet is not %s, ignoring...\n", "!PSH");
						continue;
					}
				}
				if(door->flag_ack != DONT_CARE) {
					if(door->flag_ack == SET && !TCP_HAS_FLAG(tcp, ack, TH_ACK)) {
						dprint("packet is not %s, ignoring...\n", "ACK");
						continue;
					}
					if(door->flag_ack == NOT_SET && TCP_HAS_FLAG(tcp, ack, TH_ACK)) {
						dprint("packet is not %s, ignoring...\n", "!ACK");
						continue;
					}
				}
				if(door->flag_urg != DONT_CARE) {
					if(door->flag_urg == SET && !TCP_HAS_FLAG(tcp, urg, TH_URG)) {
						dprint("packet is not %s, ignoring...\n", "URG");
						continue;
					}
					if(door->flag_urg == NOT_SET && TCP_HAS_FLAG(tcp, urg, TH_URG)) {
						dprint("packet is not %s, ignoring...\n", "!URG");
						continue;
					}
				}
			}

			if(IP_GET_PROTO(ip) == door->protocol[0] && dport == door->sequence[0]) {
				struct hostent *he;
				/* create a new entry */
				attempt = (knocker_t*)malloc(sizeof(knocker_t));
				attempt->srchost = NULL;
				if(attempt == NULL) {
					perror("malloc");
					exit(1);
				}
				strcpy(attempt->src, srcIP);
				/* try a reverse lookup if enabled  */
				if (o_lookup) {
					inaddr.s_addr = IP_GET_SADDR(ip);
					he = gethostbyaddr((void *)&inaddr, sizeof(inaddr), AF_INET);
					if(he) {
						attempt->srchost = strdup(he->h_name);
					}
				}

				attempt->stage = 1;
				attempt->seq_start = pkt_secs;
				attempt->door = door;
				if(attempt->srchost) {
					vprint("%s (%s): %s: Stage 1\n", attempt->src, attempt->srchost, door->name);
					logprint("%s (%s): %s: Stage 1", attempt->src, attempt->srchost, door->name);
				} else {
					vprint("%s: %s: Stage 1\n", attempt->src, door->name);
					logprint("%s: %s: Stage 1", attempt->src, door->name);
				}
				attempts = list_add(attempts, attempt);
			}
		}
	}
}

/* vim: set ts=2 sw=2 noet: */
