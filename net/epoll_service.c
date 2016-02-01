/*
 * epoll.c
 *
 *  Created on: 2015年12月11日
 *      Author: Random
 */

#ifdef NET_LINUX

#include <stdlib.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>

#include "net_atomic.h"
#include "../tool/ffid.h"
#include "buff.h"
#include "net_service.h"


#define Handle_EPOLLOUT 1 << 4
#define Handle_get_error 1 << 5
#define Handle_socket_close 1 << 6
#define EPOLLERR_EPOLLHUP 1 << 7
#define WriteError 1 << 8

#define OP_NET_READ				EPOLLIN
#define OP_NET_WRITE 			EPOLLOUT

 struct listen_session
{
	unsigned short				port;
	unsigned short				listen_cnt;
};

#include <signal.h>
 int net_init()
 {
 	// if read from or write to a closed socket, the sign will closed the process
 	// need igon
 	signal(SIGPIPE, SIG_IGN);
 	return 1;
 }

 void net_cleanup()
 {

 }


//ctl socket fd as async
int ctl_socket_async(NET_SOCKET fd)
{
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if(flags == -1) return -1;
	flags |= O_NONBLOCK;
	if(fcntl(fd, F_SETFL, flags) == -1) return -1;
	return 0;
}

NET_SERVICE_TYPE net_create_service_fd(int size)
{
	int fd;
	fd = epoll_create(size);
	if(fd < 0) return 0;
	return fd;
}


int epoll_ctl_op(int epfd, struct net_session* session, int op)
{
	// only call by post_read and post_write
	struct epoll_event event;
	int ev;
	int err;

	if(!session) return -1;
	ev = 0;


	if(session->rsession)
	{
		ev |= session->rsession->op;
	}
	if(session->wsession)
	{
		ev |= session->wsession->op;
	}
	if(ev & op) return 0;
	event.data.u64 = session->id;
	event.events = op | ev | EPOLLET;
	if(!ev)
	{
		err = epoll_ctl(epfd, EPOLL_CTL_ADD, session->fd, &event);
		if(err)
		{
			err = net_get_error();
			// perror ("epoll_ctl_op");
		}
		return err;
	}
	else
	{
		err = epoll_ctl(epfd, EPOLL_CTL_MOD, session->fd, &event);
		if(err)
		{
			err = net_get_error();
			// perror ("epoll_ctl_op");
		}
		return err;
	}
}

#include <stdio.h>
// #include <time.h>


#define print_error()
	// perror("	error:");
	// printf("%s:%d    %s %d %d\n", __FILE__, __LINE__, __FUNCTION__, net_get_error(), clock());
	// fflush(stdout);


#define net_socket_close_print_error() 
	// perror("	error:");
	// printf("%s:%d    %s %d %d\n", __FILE__, __LINE__, __FUNCTION__, net_get_error(), clock());
	// fflush(stdout);

static void clean_epoll_op(struct net_service* service, struct net_session* session)
{
	epoll_ctl(service->net_service_fd, EPOLL_CTL_DEL, session->fd, 0);
	if(session->wsession)
	{
		session->wsession->op = OP_NET_NONE;
	}
	if(session->rsession)
	{
		session->rsession->op = OP_NET_NONE;
	}
}

int
post_rest_write(struct net_service* service, struct net_session* session)
{
	void* data;
	struct write_session* wsession;
	size_t size;
	int ret;

	wsession = session->wsession;
	if(!wsession) return -1;

	session->wsession->etflag = 0;

	while((size = send_buff_prepare(wsession->sbuff, &data)))
	{
		ret = send(session->fd, data, size, 0);
		if(ret == -1)
		{
			if(errno == EAGAIN )
			{
				//fd buffer is full
				return errno;
			}
			else
			{
				return -errno;
			}
		}
		if(ret > 0)
		{
			send_buff_consume(wsession->sbuff, ret);
		}
	}
	session->wsession->etflag = 1;
	return 0;
}



void 
post_rest_read(struct net_service* service, struct net_session* session)
{
	void* data;
	struct read_session* rsession;
	size_t size;
	int err;
	int msgcnt;

	rsession = session->rsession;
	if(!rsession) return ;
	rsession->etflag = 0;

	for (;;)
	{
		size = recv_buff_prepare(rsession->rbuff, &data);
		if (size == 0)
		{
			rsession->etflag = 1;
			break;
		}
		err = recv(session->fd, (char*)data, (int)size, 0);
		msgcnt = recv_buff_consume(rsession->rbuff, err < 0? 0 : (size_t)err);
		if (err > 0)
		{
			if(msgcnt > 0)
			{
				push_queue(service, session, Eve_Read);
			}
		}
		else if (err == 0)
		{
			// socket closed
			print_error();
			clean_epoll_op(service, session);
			push_queue(service, session, Handle_socket_close | Eve_Error);
			break;
		}
		else
		{
			err = net_get_error();
			if (err != EAGAIN )
			{
				print_error();
				clean_epoll_op(service, session);
				push_queue(service, session, Handle_get_error | Eve_Error);
			}
			break;
		}
	}
}


int 
// post a read op
post_read(struct net_service* service, struct net_session* session)
{
	struct read_session* rsession;

	if( !service)
	{
		return -1;
	}
	if( !session->rsession )
	{
		return -1;
	}
	rsession = session->rsession;

	if(rsession->op != OP_NET_NONE)
	{
		if(session->rsession->etflag)
		{
			post_rest_read(service, session);
		}
		return 0;
	}

	if(epoll_ctl_op(service->net_service_fd, session, OP_NET_READ))
	{
		print_error();
		push_queue(service, session, Eve_Read | Eve_Error);
		clean_epoll_op(service, session);
	}
	else
	{
		rsession->op = OP_NET_READ;
	}
	return 1;
}

// post a write op
int 
post_write(struct net_service* service, struct net_session* session)
{
	struct write_session* wsession;
	void* data;
	size_t size;

	if(!service)
	{
		return -1;
	}

	if(!session->wsession)
	{
		return -1;
	}
	wsession = session->wsession;
	if(wsession->op != OP_NET_NONE)
	{
		post_rest_write(service, session);
		return 0;
	}
	size = send_buff_prepare(wsession->sbuff, &data);
	if(size == 0)
	{
		return 0;
	}
	
	if(epoll_ctl_op(service->net_service_fd, session, OP_NET_WRITE))
	{
		wsession->op = OP_NET_NONE;
		print_error();
		push_queue(service, session, WriteError | Eve_Error);
		clean_epoll_op(service, session);

	}
	else
	{
		wsession->op = OP_NET_WRITE;
	}
	return 1;
}


void
release_rest_session(struct net_service* service, struct net_session* session)
{
	net_lock(&service->close_lock);
	sb_tree_delete(&service->close_root, session->id);
	net_unlock(&service->close_lock);

	clean_epoll_op(service, session);

	net_close_fd(session->fd);
	release_net_session(session);
}

void
handle_rest(struct net_service* service, struct net_session* session, int events)
{
	if(!session) return;
	if((events & EPOLLERR) || (events & EPOLLHUP))
	{
		release_rest_session(service, session);
		return;
	}

	if(events & EPOLLOUT)
	{
		if( !session->wsession || !session->wsession->send_rest || post_rest_write(service, session) <= 0)
		{
			release_rest_session(service, session);
			return;
		}
	} else if(!(events & EPOLLIN))
	{
		release_rest_session(service, session);
		return;
	}
}



void
handle_session(struct net_service* service, struct net_session* session, int events)
{
	unsigned int event;
	int error;
	int err;

	error = 0;
	event = 0;

	socklen_t len = sizeof(int);

	if(!session) return;

	if(session->lsession)
	{
		error = 1;
	}

	// printf("%d %d %d %d %d %d %d\n", session->id, session->events, error, events & EPOLLERR, events & EPOLLHUP, events & EPOLLOUT, events & EPOLLIN);
	// fflush(stdout);
	if(session->lsession)
	{
		// listen session
		push_queue(service, session, Eve_Accept);
		return;
	}

	if(session->connect_flag)
	{
		event |= Eve_Connect;
	}

	if((events & EPOLLERR) || (events & EPOLLHUP))
	{
		print_error();
		clean_epoll_op(service, session);
		push_queue(service, session, event | EPOLLERR_EPOLLHUP | Eve_Error);
		return;
	}

	if(session->connect_flag)
	{
		if(events & EPOLLOUT)
		{
			if (( 0 == getsockopt(session->fd, SOL_SOCKET, SO_ERROR, &error, &len) )) {
				if( 0 == error ) {
					clean_epoll_op(service, session);
					push_queue(service, session, Eve_Connect);
					return;
				}
			}
			// maybe error
			print_error();
			clean_epoll_op(service, session);
			push_queue(service, session, Eve_Connect | Eve_Error);
		}
		return;
	}

	if(events & EPOLLOUT)
	{
		err = post_rest_write(service, session);
		// service->handle_msg_cnt += 1;
		if(err < 0)
		{
			print_error();
			clean_epoll_op(service, session);
			push_queue(service, session, Handle_EPOLLOUT | Eve_Error);
		}
		else if(err == 0)
		{
			//no have need write data
			
		}
	}

	if(events & EPOLLIN )
	{
		post_rest_read(service, session);
	}
}



#include <stdio.h>


#define print_lock_help(index ) \
	printf("%s:%d    %s %d\n", __FILE__, __LINE__, __FUNCTION__, index);\
	fflush(stdout);

int
net_wait(struct net_service* service, int timeout)
{
	int cnt;
	int wait_cnt;
	int i;
	ffid_vtype id;
	struct epoll_event events[32];
	struct net_session* session;
	const struct sbtree_node* node;
	unsigned short index;


	cnt = 0;

	while((wait_cnt = epoll_wait(service->net_service_fd, events, sizeof(events)/sizeof(events[0]), timeout)))
	{
		if(wait_cnt < 0) return wait_cnt;
		cnt += wait_cnt;
		for(i = 0; i < wait_cnt; ++i)
		{

			id = (ffid_vtype)events[i].data.u64;
			if(!id) continue;
			index = ffid_index(service->socket_ids, id);

			net_lock(&service->session_lock[index]);
			session = service->sessions[index];
			if(!session || session->id != id)
			{
				//session delete from net_service
				net_lock(&service->close_lock);
				node = sb_tree_find(service->close_root, id);
				net_unlock(&service->close_lock);
				if(node)
				{
					session = (struct net_session*)node->value.ptr;
					handle_rest(service, session, events[i].events);
				}
				else
				{
					// cur thread get the id but threadB close session and no write op
				}
			}
			else
			{
				//session still in net_service
				handle_session(service, session, events[i].events);
			}
			net_unlock(&service->session_lock[index]);
		}
		
	}
	return cnt;
}

void
release_listen_session(struct listen_session* lsession)
{
	if(!lsession)
	{
		return;
	}
	free(lsession);
}

struct listen_session*
create_listen_session(struct net_service* service, unsigned short port, unsigned short listen_cnt)
{
	struct listen_session* lsession;
	lsession = (struct listen_session*)(malloc(sizeof(struct listen_session)));
	if(!lsession)
	{
		return 0;
	}
	memset(lsession, 0, sizeof(struct listen_session));
	lsession->listen_cnt = listen_cnt;
	lsession->port = port;
	return lsession;
}

net_socket
net_listen(struct net_service* service, unsigned short port, unsigned short listen_cnt)
{
	int listen_socket;
	int opt;
	struct sockaddr_in listen_addr;
	struct epoll_event ev;
	struct net_session* session;
	int ret;
	ffid_vtype id;

	listen_socket = create_socket();
	if(listen_socket < 0)
	{
		// printf("listen error step 0 %d\n", ret);
		// perror("info:");
		return 0;
	}
	opt = 1;
	// listen socket reuse
	setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	ctl_socket_async(listen_socket);

	listen_addr.sin_family = AF_INET;
	listen_addr.sin_port = htons(port);
	listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if((ret = bind(listen_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr))))
	{
		ret = net_get_error();
		// printf("listen error step 1 %d\n", ret);
		// perror("info:");
		net_close_fd(listen_socket);

		return 0;
	}

	if((ret = listen(listen_socket, listen_cnt)))
	{
		ret = net_get_error();
		// printf("listen error step 2 %d\n", ret);
		// perror("info:");
		net_close_fd(listen_socket);
		return 0;
	}

	session = create_net_session();
	if(!session)
	{
		// printf("listen error step 3\n");
		net_close_fd(listen_socket);
		return 0;
	}
	session->fd = listen_socket;
	session->lsession = create_listen_session(service, port, listen_cnt);
	if(!session->lsession)
	{
		// printf("listen error step 4 \n");
		release_net_session(session);
		net_close_fd(listen_socket);
		return 0;
	}

	id = add_net_session(service, session);
	if(!id)
	{
		// printf("listen error step 5 \n");
		release_net_session(session);
		net_close_fd(listen_socket);
		return 0;
	}

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
	ev.data.u64 = session->id;
	if((ret = epoll_ctl(service->net_service_fd, EPOLL_CTL_ADD, listen_socket, &ev)))
	{
		ret = net_get_error();
		// printf("listen error step 6 %d\n", ret);
		// perror("info:");
		net_socket_close_print_error();
		net_socket_close(service, id, 0);
		return 0;
	}
	return id;
}

net_socket
net_accept(struct net_service* service, net_socket nd)
{
	unsigned short index;
	struct net_session* session;
	struct net_session* new_session;
	int fd;
	ffid_vtype id;

	if(!service || !nd) return 0;

	index = ffid_index(service->socket_ids, nd);
	net_lock(&service->session_lock[index]);
	session = service->sessions[index];
	if(!session ||  !session->lsession || session->id != nd)
	{
		net_unlock(&service->session_lock[index]);
		return 0;
	}
	fd = accept(session->fd, 0, 0);
	if(fd < 0)
	{
		net_unlock(&service->session_lock[index]);
		
		return 0;
	}
	ctl_socket_async(fd);
	new_session = create_net_session();
	if(!new_session)
	{
		net_close_fd(fd);
		net_unlock(&service->session_lock[index]);
		return 0;
	}
	net_unlock(&service->session_lock[index]);

	new_session->fd = fd;

	id = add_net_session(service, new_session);
	if(!id)
	{
		release_net_session(new_session);
		net_close_fd(fd);
		return 0;
	}
	return id;
}

net_socket
net_connect(struct net_service* service, const char* ip, unsigned short port)
{
	int connect_socket;
	struct sockaddr_in connect_addr;
	struct epoll_event ev;
	struct net_session* session;
	int ret;
	ffid_vtype id;

	connect_socket = create_socket();
	if(connect_socket < 0)
	{
		return 0;
	}
	ctl_socket_async(connect_socket);


	session = create_net_session();
	if(!session)
	{
		net_close_fd(connect_socket);
		return 0;
	}
	session->fd = connect_socket;
	session->connect_flag = 1;

	id = add_net_session(service, session);
	if(!id)
	{
		release_net_session(session);
		net_close_fd(connect_socket);
		return 0;
	}

	memset(&connect_addr, 0, sizeof(connect_addr));
	connect_addr.sin_family = AF_INET;
	connect_addr.sin_addr.s_addr = inet_addr(ip);
	connect_addr.sin_port = htons(port);

	ret = connect(connect_socket, (struct sockaddr*)&connect_addr, sizeof(connect_addr));
	if(ret < 0)
	{
		ret = net_get_error();
		if(ret != EINPROGRESS)
		{
			net_socket_close_print_error();
			net_socket_close(service, id, 0);
			return 0;
		}
	}

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
	ev.data.u64 = session->id;
	if((ret = epoll_ctl(service->net_service_fd, EPOLL_CTL_ADD, connect_socket, &ev)))
	{
		ret = net_get_error();
		net_socket_close_print_error();
		net_socket_close(service, id, 0);
		return 0;
	}

	return id;
}

void
net_socket_close(struct net_service* service, net_socket nd, char send_rest)
{
	unsigned short index;
	struct net_session* session;
	sb_tree_value v;

	if(!service)
	{
		return;
	}
	if(!nd)
	{
		return;
	}

	index = ffid_index(service->socket_ids, nd);
	net_lock(&service->session_lock[index]);
	session = service->sessions[index];
	if(!session || session->id != nd)
	{
		net_unlock(&service->session_lock[index]);
		return;
	}
	// epoll_ctl(service->net_service_fd, EPOLL_CTL_DEL, session->fd, 0);
	clean_epoll_op(service, session);
	if(send_rest)
	{
		post_write(service, session);
	}
	
	service->sessions[index] = 0;
	// release read session
	release_read_session(session->rsession);
	session->rsession = 0;

	if(session->wsession && session->wsession->op == OP_NET_WRITE)
	{
		session->wsession->send_rest = send_rest;
		shutdown(session->fd, 0);
		release_listen_session(session->lsession);
		session->lsession = 0;
		// join to close_root sb_tree
		net_lock(&service->close_lock);
		v.ptr = session;
		sb_tree_insert(&service->close_root , nd, v);
		net_unlock(&service->close_lock);
	}
	else
	{
		net_close_fd(session->fd);
		release_net_session(session);
	}
	net_unlock(&service->session_lock[index]);

	net_lock(&service->id_lock);
	ffid_del_id(service->socket_ids, nd);
	net_unlock(&service->id_lock);
}


#include <unistd.h>
void
net_service_sleep(long ms)
{
	usleep(ms * 1000);
}


#endif //NET_LINUX


