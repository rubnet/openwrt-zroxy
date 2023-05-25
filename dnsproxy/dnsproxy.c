/*
 * dnsproxy.c
 *
 *  Created on: Jun 3, 2021
 *      Author: zeus
 */
#include "dnsproxy.h"
#include <fcntl.h>
#include <netdb.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <log/log.h>
#include <stdbool.h>
#include <sys/time.h>
#include <errno.h>
#include <unistd.h>
#include <socks.h>
#include <pthread.h>
#include <dns.h>
#include <unistd.h>
#include <net.h>
#include <filter/filter.h>
#include <time.h>


void *dnsserver_workerTask(void *vargp);

typedef struct
{
	dnsserver_t	*dns;
	dnsMessage_t msg;
}dnsThread_t;

dnsserver_t *localdns_init_config(dnshost_t *conf)
{
	dnsserver_t *ptr = (dnsserver_t*)malloc(sizeof(dnsserver_t));
	if(!ptr)
		return NULL;
	bzero(ptr,sizeof(dnsserver_t));

	/* fifo init*/
	ptr->fifo = (fifo_t*)malloc(sizeof(fifo_t));
	if(!fifo_init(ptr->fifo,D_FIFO_Message_Size,D_FIFO_Item))
	{
		log_error("[!] Error DNS fifo not Set");
		localdns_free(ptr);
		return NULL;
	}

	strcpy(ptr->listen_addr,conf->Local.ip);
	sprintf(ptr->listen_port,"%d",conf->Local.port);
	ptr->socks = conf->Socks;

	/*init upstream dns server*/
	ptr->upstream = conf->Remote;

	// copy stat handler
	ptr->Stat = conf->Stat;

	// copy withlist status;
	ptr->whitelist = conf->whitelist;

	// copy sni ip
	memcpy(ptr->sni_ip,conf->sni_ip,4);

	/*Create Worker Task*/
	pthread_t thread_id;
	pthread_create(&thread_id, NULL, dnsserver_workerTask, (void*)ptr);

	return ptr;
}

void localdns_free(dnsserver_t *dns)
{
	fifo_free(dns->fifo);
	free(dns->socks);
	free(dns);
}

bool localdns_init_sockets(dnsserver_t *dns)
{
  struct addrinfo hints;
  struct addrinfo *addr_ip;
  int r;

  dns->local_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (0 != (r = getaddrinfo(dns->listen_addr, dns->listen_port, &hints, &addr_ip)))
  {
    log_error("%s:%s:%s\n", gai_strerror(r), dns->listen_addr, dns->listen_port);
    return false;
  }

  if (0 != bind(dns->local_sock, addr_ip->ai_addr, addr_ip->ai_addrlen))
  {
    log_error("Can't bind address %s:%s\n", dns->listen_addr, dns->listen_port);
    return false;
  }

  freeaddrinfo(addr_ip);
  return true;
}

bool localdns_pull(dnsserver_t *dns)
{
	socklen_t client_size = sizeof(struct sockaddr_in);

	dnsMessage_t	msg;
	// receive a dns request from the client
	msg.len = recvfrom(dns->local_sock, &msg.message[2], DNS_MSG_SIZE - 2, 0, (struct sockaddr *)&msg.client, &client_size);

	// lets not fork if recvfrom was interrupted
	if (msg.len < 0 && errno == EINTR) 
	{ 
		return true; 
	}

	// other invalid values from recvfrom
	if (msg.len < 0)
	{
		log_error("recvfrom failed: %s\n", strerror(errno));
		return true;
	}

	// the tcp query requires the length to precede the packet, so we put the length there
	msg.message[0] = (msg.len>>8) & 0xFF;
	msg.message[1] = msg.len & 0xFF;

	/*send dns message to dns fifo*/
	if(fifo_incert(dns->fifo,&msg))
	{
		if(dns->Stat)
			state_IncConnection(dns->Stat);
	}
	else
	{
		log_error("dns fifo is full:");
	}

	return true;
}

/* Return Len of packet*/
int dns_resolve_query(dnsserver_t *dns,struct Message *msg,uint8_t *buf)
{
	struct ResourceRecord *rr = malloc(sizeof(struct ResourceRecord));
	bzero(rr,sizeof(struct ResourceRecord));

	rr->name = strdup(msg->questions->qName);
	rr->type = msg->questions->qType;
	rr->class = msg->questions->qClass;
	rr->ttl = 60*60; // in seconds; 0 means no caching

	rr->rd_length = 4;
	rr->rd_data.a_record.addr[0] = dns->sni_ip[0];
    rr->rd_data.a_record.addr[1] = dns->sni_ip[1];
    rr->rd_data.a_record.addr[2] = dns->sni_ip[2];
    rr->rd_data.a_record.addr[3] = dns->sni_ip[3];

	// leave most values intact for response
	msg->qr = 1; // this is a response
	msg->aa = 1; // this server is authoritative
	msg->ra = 0; // no recursion available
	msg->rcode = Ok_ResponseType;
	
	// should already be 0
	msg->anCount = 0;
	msg->nsCount = 0;
	msg->arCount = 0;

	msg->anCount++;

	// prepend resource record to answers list
    msg->answers = rr;
    
	uint8_t *p = buf;
	if (!dns_encode_msg(msg, &p)) 
	{
    	return 0;
    }

	return p - buf;
}

const char *DNS_GetType(uint16_t type)
{
  switch(type)
  {
  	case A_Resource_RecordType: return "A";
  	case NS_Resource_RecordType: return "NS";
  	case CNAME_Resource_RecordType: return "CNAME";
  	case SOA_Resource_RecordType: return "SOA";
  	case PTR_Resource_RecordType: return "PTR";
  	case MX_Resource_RecordType:  return "MX";
  	case TXT_Resource_RecordType: return "TXT";
  	case AAAA_Resource_RecordType: return "AAAA";
  	case SRV_Resource_RecordType: return "SRV";
	default:	return "unknow";
  }
  return "";
}

uint64_t timeInMilliseconds(void) 
{
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((uint64_t)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}


void *DNS_HandleIncomingRequset(void *ptr)
{

	dnsserver_t *dns = ((dnsThread_t*)ptr)->dns;
	dnsMessage_t *msg = &((dnsThread_t*)ptr)->msg;
	
	
	char domain_resolve[1024] = {0};
	

	uint64_t start_time = timeInMilliseconds(); 

	int sockssocket = 0;
	do
	{
		/*Check and Print DNS Question*/
		struct Message dns_msg = {0};
		if(dns_decode_msg(&dns_msg, &msg->message[2], msg->len))
		{
			struct Question *q;
			q = dns_msg.questions;
			log_info("DNS Question { (%s) qName '%s'}",DNS_GetType(q->qType),q->qName);
			snprintf(domain_resolve,1023,"resolve { (%s) qName '%s'} ",DNS_GetType(q->qType),q->qName);

			/*try make replay*/
			if(dns->whitelist && q->qType == A_Resource_RecordType &&
				filter_IsWhite(dns->whitelist,q->qName))
			{
				uint8_t buffer[DNS_MSG_SIZE];
				int len = dns_resolve_query(dns,&dns_msg,buffer);
				/*if packet fill send to network*/
				if(len)
				{
					// send the reply back to the client
					sendto(dns->local_sock, buffer, len, 0, (struct sockaddr *)&msg->client, sizeof(struct sockaddr_in));
					log_info("DNS local replay");

					uint64_t End_Time = timeInMilliseconds() - start_time;
					double time_taken = ((double)End_Time)/(1000); // convert to sec
					log_info("%s in %0.3fs",domain_resolve,time_taken);

					//Update statistics
					if(dns->Stat)
					{
						state_RxTxClose(dns->Stat,len,msg->len);
					}
					free_msg(&dns_msg);
					break;
				}
			}
		}
		free_msg(&dns_msg);


		/*forward packet to server via socks*/
		int rlen = 0;
		int trysend = 2;
		while (trysend--)
		{
			if(dns->socks)
			{
				// make socks5 socket
				if(!socks5_connect(&sockssocket,dns->socks, dns->upstream.ip, dns->upstream.port,true))
					break;
			}
			else
			{
				/*direct connect to dns server*/
				if(!net_connect(&sockssocket, dns->upstream.ip, dns->upstream.port))
					break;
			}
			
			/*set socket timeout*/
			if(!net_socket_timeout(sockssocket,5,5))
			{
				log_error("DNS: can not set socket timeout");
			}

			// forward dns query
			if(send(sockssocket, msg->message, msg->len + 2,MSG_NOSIGNAL)<0)
			{
				/*maybe socket is not connect*/
				//close(sockssocket);
				//continue;
				break;
			}

			rlen = read(sockssocket, msg->message, DNS_MSG_SIZE);
			if(rlen<=0)
			{
				/*maybe socket is not connect*/
				//close(sockssocket);
				//continue;
			}

			break;
		}

		/*close sockst*/
		close(sockssocket);

		if(rlen>0)
		{
			log_info("DNS SEND %i and GET %i",msg->len,rlen);

			// forward the packet to the tcp dns server
			// send the reply back to the client (minus the length at the beginning)
			sendto(dns->local_sock, msg->message + 2, rlen - 2 , 0, (struct sockaddr *)&msg->client, sizeof(struct sockaddr_in));

			uint64_t End_Time = timeInMilliseconds() - start_time;
					double time_taken = ((double)End_Time)/(1000); // convert to sec
			log_info("%s in %0.3fs",domain_resolve,time_taken);
		}
		else
		{
			uint64_t End_Time = timeInMilliseconds() - start_time;
			double time_taken = ((double)End_Time)/(1000); // convert to sec
			log_error("NO DATA %s in %0.3fs",domain_resolve,time_taken);
		}

		//Update statistics
		if(dns->Stat)
		{
			state_RxTxClose(dns->Stat,rlen,msg->len+2);
		}

	}while(0);

	free(ptr);
	pthread_exit(0);
	return 0;
}

void *dnsserver_workerTask(void *vargp)
{
	dnsserver_t *dns = (dnsserver_t*) vargp;
	dnsMessage_t msg;
	
	log_info("DNS Worker Started");
	while(1)
	{
		if(!fifo_Read(dns->fifo,&msg))
		{
			usleep(10000);
			continue;
		}
		
		
		
		/*clone message*/
		dnsThread_t *newmsg = malloc(sizeof(dnsThread_t));
		memcpy(&newmsg->msg,&msg,sizeof(dnsMessage_t));
		newmsg->dns = dns;
		/*Process incoming dns message*/
		pthread_t thread_id;
		pthread_create(&thread_id, NULL, DNS_HandleIncomingRequset, (void*)newmsg);
		pthread_detach(thread_id);
	}

	return NULL;
}