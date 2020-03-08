/*
 * monitor.h
 *
 *  Created on: Feb 17, 2020
 *      Author: zeus
 */
#include <stdbool.h>
#include "statistics.h"

typedef struct
{
	int fd;
	uint16_t Port;
	statistics_t *state;
}mon_t;

typedef struct
{
	int		 			connid;
	struct sockaddr_in	cli;
	socklen_t 			addr_len;
	statistics_t 		*state;
}monclient_t;

typedef struct
{
	uint16_t tm_yday;
	uint8_t  tm_hour;
	uint8_t  tm_min;
	uint8_t  tm_sec;
}UpTime_t;

statistics_t *monitor_Init(uint16_t *Port);

