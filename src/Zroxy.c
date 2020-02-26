/*
 ============================================================================
 Name        : Zroxy.c
 Author      : Zeus
 Version     :
 Copyright   : Copyright 2020 Mohammad Mazarei This program is free software
 Description : simple and smart sni-proxy.
 ============================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h> /*Lib Socket*/
#include <unistd.h>  	/*Header file for sleep(). man 3 sleep for details.*/
#include <pthread.h>
#include <log/log.h>
#include <SniProxy.h>
#include "version.h"
#include <netdb.h>		/* getaddrinfo, getnameinfo */ /*https://rosettacode.org/wiki/DNS_query#C*/

#include <argp.h>
#include <stdbool.h>
#include "args.h"
#include "statistics/monitor.h"

zroxy_t prg_setting = {0};


/*
 * about tcp/ip and socket in c:
 * https://www.csd.uoc.gr/~hy556/material/tutorials/cs556-3rd-tutorial.pdf
 *
 * for Non Block IO:
 * https://www.gnu.org/software/libc/manual/html_node/Waiting-for-I_002fO.html
 * https://www.gnu.org/software/libc/manual/html_node/Server-Example.html
 * https://www.tenouk.com/Module41.html
 * */
int main(int argc, const char **argv)
{

	Log_init();
	arg_Init(&prg_setting,argc,argv);

	statistics_t *state = NULL;
	/*check Monitor*/
	if(prg_setting.monitorPort)
	{
		log_info("enable monitor on port %i",*prg_setting.monitorPort);
		state = monitor_Init(prg_setting.monitorPort);
	}

	lport_t *p=prg_setting.ports;
	SniServer_t Xconf = { .Port = 443, .Socks = NULL ,.sta = state};
	if(prg_setting.socks)	/*if Set Socks proxy*/
	{
		log_info("enable socks on %s:%i",prg_setting.socks->host,prg_setting.socks->port);
		Xconf.Socks = prg_setting.socks;
	}

	while(p)
	{
		Xconf.Port = p->port;
		SniProxy_Start(&Xconf);
		p=p->next;
	}
	Free_PortList(&prg_setting); /*free Port List*/

	log_trace("exit from main thread");
	pthread_exit(NULL);		/* terminate the thread */
	//return 0;
}

