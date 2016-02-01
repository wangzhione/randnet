


#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "../net/buff.h"
#include "../net/net_service.h"

#include "pthread.h"

int msg_send_count = 0;
int msg_recv_count = 0;


void*
thread_run(void* param)
{
	struct net_service* ns;
	ns = (struct net_service*)param;
	while(1)
	{
		net_wait(ns, 100);
	}
	
	return 0;
}

void handle_client_read(struct net_service* ns, net_socket nd)
{
	char buff[1024];
	int ret;

	while((ret = net_socket_read(ns, nd, buff, sizeof(buff)/sizeof(buff[0]))) > 0)
	{
		if(ret < (int)(sizeof(buff)/sizeof(buff[0])) )
		{
			buff[ret] = 0;
			// printf("recv msg %s\n", buff);
			++msg_recv_count;
			if( net_socket_write(ns, nd, buff, ret) > 0)
			{
				++msg_send_count;
			}
		}
		else
		{
			// create lager buff，read again
			break;
		}
	}
}



int main(int argc, char** argv)
{
	struct net_service* ns;
	pthread_t ts[2];
	struct net_event events[64];
	struct buff_pool* pool;
	int i;
	int ret;
	void* status;
	unsigned int e;
	struct net_config cfg;
	int count;
	param_type parm;
	char bf[1024];

	srand((unsigned)time(NULL));


	pool = buff_pool_create(1024, 64);
	if(!pool)
	{
		printf("buff_pool_create faild\n");
		exit(-1);
	}

	ns = net_create(1024 );
	if(!ns)
	{
		printf("open net_service faild\n");
		exit(-1);
	}

	for(i = 0; i < (int)(sizeof(ts)/sizeof(ts[0])); ++i)
	{
		pthread_create(ts + i, 0, thread_run, ns);
	}

	count = 0;

	while(1)
	{
		ret = net_queue(ns, events, sizeof(events)/sizeof(events[0]));
		if(ret < 0)
		{
			break;
		}
		if(ret == 0)
		{
			net_service_sleep(10);
			++count;
			
			if(!net_connect(ns, "127.0.0.1", 9522))
			{
				// printf("connect error\n");
				fflush(stdout);
			}
			
			if(count % 100 == 0)
			{
				printf("-------------------------------- %d %d %d\n", net_socket_size(ns), msg_send_count, msg_recv_count);
				fflush(stdout);
			}
			continue;
		}


		for(i = 0; i < ret; ++i)
		{
			e = events[i].events;
			if(e & Eve_Error)
			{
				printf("socket error %d  %d\n", events[i].nd, e);
				fflush(stdout);
				net_socket_close(ns, events[i].nd, 0);
				continue;
			}


			if(e & Eve_Connect)
			{
				cfg.enByte = enByte16;
				cfg.read_buff_cnt = 8;
				cfg.write_buff_cnt = 8;
				cfg.pool = pool;

				if(net_socket_cfg(ns, events[i].nd, &cfg) < 0)
				{
					net_socket_close(ns, events[i].nd, 0);
				}
				else
				{
					// add a connect
					parm = 1;
					net_socket_ctl(ns, events[i].nd , &parm);
					sprintf(bf, " s %d do some read write op  %d %d\n", net_socket_size(ns), rand(), rand());
					if(net_socket_write(ns, events[i].nd, bf, strlen(bf) + 1) > 0)
					{
						++msg_send_count;
					}
				}
			}
			
			if(e & Eve_Read)
			{
				parm = net_socket_ctl(ns, events[i].nd, 0);
				if((rand() %  20480) <  ((int)parm / 256) )
				{
					net_socket_close(ns, events[i].nd, 1);
				}
				else
				{
					++parm;
					net_socket_ctl(ns, events[i].nd, &parm);
					handle_client_read(ns, events[i].nd);
				}
			}
		}
	}

	for(i = 0; i < (int)(sizeof(ts)/sizeof(ts[0])); ++i)
	{
		pthread_join(ts[i], &status);
	}
	net_close(ns);
	buff_pool_release(pool);
	return 0;
}

