/*
 * socket_server.c
 *
 *  Created on: Mar 23, 2017
 *      Author: Clemens Kresser
 *      License: MIT
 */

#include "socket_server.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include "../utils/dyn_buffer.h"
#include "../utils/ref_count.h"
#include <errno.h>
#include "../utils/log.h"
#include <netdb.h>
#include <netinet/tcp.h>


#define READ_SIZE 1024


enum socket_client_state
{
  SOCKET_CLIENT_STATE_CONNECTED,
  SOCKET_CLIENT_STATE_DISCONNECTED,
};

struct socket_client_desc
{
  volatile enum socket_client_state state;
  int clientSocketFd;
  struct socket_desc *socketDesc;
  pthread_t tid;
  struct dyn_buffer buffer;
  void *client_data;
};

struct socket_client_list_element
{
  struct socket_client_desc *desc;
  struct socket_client_list_element *next;
};

struct socket_desc
{
  struct socket_client_list_element *list;
  pthread_mutex_t mutex;
  size_t (*socket_onMessage)(void *socketUserData, void *clientDesc, void *clientUserData, void *msg, size_t len);
  void* (*socket_onOpen)(void *socketUserData, void *clientDesc);
  void (*socket_onClose)(void *socketUserData, void *clientDesc, void *clientUserData);
  void *socketUserData;
  int socketFd;
  fd_set readfds;
  volatile bool running;
  pthread_t tid;
  unsigned long numConnections;
};

/**
 * \brief: adds a client to the list of the socket descriptor
 *
 * \param *socketDesc: pointer to the socket descriptor
 * \param *desc: pointer to the client descriptor that should be added
 *
 * \return: 0 if successful else -1
 */
static int addClient(struct socket_desc *socketDesc, struct socket_client_desc *desc)
{
  struct socket_client_list_element *listElement;

  listElement = malloc(sizeof(struct socket_client_list_element));
  if(!listElement)
  {
    log_err("malloc failed");
    return -1;
  }

  refcnt_ref(desc);
  listElement->desc = desc;

  pthread_mutex_lock(&socketDesc->mutex);
  {
    if(!socketDesc->list)
    {
      listElement->next = NULL;
      socketDesc->list = listElement;
    }
    else
    {
      listElement->next = socketDesc->list;
      socketDesc->list = listElement;
    }
    socketDesc->numConnections++;
  }
  pthread_mutex_unlock(&socketDesc->mutex);

  return 0;
}

/**
 * \brief: removes a client from the list of the socket descriptor
 *
 * \param *socketDesc: pointer to the socket descriptor
 * \param *desc: pointer to the client descriptor that should be removed
 *
 * \return: 0 if successful else -1
 */
static int removeClient(struct socket_desc *socketDesc, struct socket_client_desc *desc)
{
  struct socket_client_list_element *listElement, *previousListElement = NULL;
  int rc = -1;

  pthread_mutex_lock(&socketDesc->mutex);
  {
    listElement = socketDesc->list;

    while(listElement)
    {
      if(listElement->desc == desc)
      {
        if(!previousListElement)
          socketDesc->list = listElement->next;
        else
          previousListElement->next = listElement->next;
        free(listElement);
        rc = 0;
        socketDesc->numConnections--;
        break;
      }
      previousListElement = listElement;
      listElement = listElement->next;
    }
  }
  pthread_mutex_unlock(&socketDesc->mutex);
  refcnt_unref(desc);

  return rc;
}

/**
 * \brief: closes all connections
 *
 * \param *socketDesc: pointer to the socket descriptor
 */
static void closeAllClients(struct socket_desc *socketDesc)
{
  struct socket_client_list_element *listElement, *prevListElement;

  pthread_mutex_lock(&socketDesc->mutex);
  {
    listElement = socketDesc->list;
    while(listElement)
    {
      prevListElement = listElement;
      listElement = listElement->next;
      socketDesc->list = listElement;
      socketServer_closeClient(prevListElement->desc);
    }
  }
  pthread_mutex_unlock(&socketDesc->mutex);
}


/**
 * \brief: client thread
 *
 * \param *params: pointer to the clientDesc (steals the reference)
 *
 * \return: NULL
 */
static void *clientThread(void *params)
{
  struct socket_client_desc *clientDesc = params;
  fd_set readfds;
  int n;
  size_t count;
  int increase;
  size_t bytesFree;
  bool first;
  struct timeval tv;

  pthread_detach(pthread_self());

  clientDesc->client_data = clientDesc->socketDesc->socket_onOpen(clientDesc->socketDesc->socketUserData, clientDesc);

  FD_ZERO(&readfds);
  FD_SET(clientDesc->clientSocketFd, &readfds);
  while(clientDesc->state == SOCKET_CLIENT_STATE_CONNECTED)
  {
    tv.tv_sec = 0;
    tv.tv_usec = 300000;
    FD_ZERO(&readfds);
    FD_SET(clientDesc->clientSocketFd, &readfds);
    if(select(clientDesc->clientSocketFd + 1, &readfds, NULL, NULL, &tv) > 0)
    {
      if(FD_ISSET(clientDesc->clientSocketFd, &readfds))
      {
        first = true;
        increase = 1;
        do
        {
          bytesFree = DYNBUFFER_BYTES_FREE(&clientDesc->buffer);
          if(DYNBUFFER_BYTES_FREE(&clientDesc->buffer) < READ_SIZE)
          {
            dynBuffer_increase_to(&(clientDesc->buffer), READ_SIZE * increase);
            bytesFree = DYNBUFFER_BYTES_FREE(&clientDesc->buffer);
            increase++;
          }
          n = recv(clientDesc->clientSocketFd, DYNBUFFER_WRITE_POS(&(clientDesc->buffer)), bytesFree, MSG_DONTWAIT);
          if(first && (n == 0))
          {
            clientDesc->state = SOCKET_CLIENT_STATE_DISCONNECTED;
            break;
          }
          first = false;

          if(n >= 0)
            DYNBUFFER_INCREASE_WRITE_POS((&(clientDesc->buffer)), n);
          else
            break;
        } while((n == bytesFree) && (clientDesc->state == SOCKET_CLIENT_STATE_CONNECTED));

        if(clientDesc->state == SOCKET_CLIENT_STATE_CONNECTED)
        {
          do
          {
            count = clientDesc->socketDesc->socket_onMessage(clientDesc->socketDesc->socketUserData, clientDesc, clientDesc->client_data,
                DYNBUFFER_BUFFER(&(clientDesc->buffer)), DYNBUFFER_SIZE(&(clientDesc->buffer)));
            dynBuffer_removeLeadingBytes(&(clientDesc->buffer), count);
          } while(count && DYNBUFFER_SIZE(&(clientDesc->buffer)) && (clientDesc->state == SOCKET_CLIENT_STATE_CONNECTED));
        }
      }
    }
  }

  dynBuffer_delete(&(clientDesc->buffer));

  clientDesc->state = SOCKET_CLIENT_STATE_DISCONNECTED;
  clientDesc->socketDesc->socket_onClose(clientDesc->socketDesc->socketUserData, clientDesc, clientDesc->client_data);
  close(clientDesc->clientSocketFd);
  removeClient(clientDesc->socketDesc, clientDesc);

  refcnt_unref(clientDesc);

  return NULL;
}

/**
 * \brief: starts a new client connection
 *
 * \param socketFd: the socket file descriptor
 * \param socketDesc: pointer to the socket descriptor
 *
 * \return 0 if successful else -1
 */
static int startClient(int socketFd, struct socket_desc *socketDesc)
{
  struct socket_client_desc *desc;

  desc = refcnt_allocate(sizeof(struct socket_client_desc), NULL);
  if(!desc)
  {
    log_err("refcnt_allocate failed");
    return -1;
  }

  desc->clientSocketFd = socketFd;
  desc->socketDesc = socketDesc;
  dynBuffer_init(&(desc->buffer));
  desc->client_data = NULL;

  addClient(socketDesc, desc);
  desc->state = SOCKET_CLIENT_STATE_CONNECTED;

  if(pthread_create(&desc->tid, NULL, clientThread, desc) != 0)
  {
    log_err("pthread_create failed");
    refcnt_unref(desc);
    return -1;
  }

  return 0;
}

/**
 * \brief: closes the given client
 *
 * \param *socketDesc: pointer to the socket descriptor
 * \param *desc: pointer to the client descriptor
 */
void socketServer_closeClient(void *socketClientDesc)
{
  struct socket_client_desc *clientDesc = socketClientDesc;
  refcnt_ref(clientDesc);
  clientDesc->state = SOCKET_CLIENT_STATE_DISCONNECTED;
  refcnt_unref(clientDesc);
}

/**
 * \brief: sends the given data over the given socket
 *
 * \param *clientDesc: pointer to the client descriptor
 * \param *msg: pointer to the data
 * \param len: the length of the data
 *
 * \return: 0 if successful else -1
 */
int socketServer_send(void *socketClientDesc, void *msg, size_t len)
{
  struct socket_client_desc *clientDesc = socketClientDesc;
  int rc;
  if(clientDesc->state == SOCKET_CLIENT_STATE_DISCONNECTED)
    return -1;

  rc = send(clientDesc->clientSocketFd, msg, len, MSG_NOSIGNAL);
  if(rc == -1)
  {
    log_err("send failed: %s", strerror(errno));
  }
  return (rc == len ? 0 : -1);
}

#define BUFSIZE 20

/**
 * \brief: processes connection requests
 *
 * \param *socketDesc: pointer to the socket descriptor
 *
 * \return: NULL
 *
 */
static void *socketServerThread(void *sockDesc)
{
  struct socket_desc *socketDesc = sockDesc;
  int socketChildFd;
  socklen_t clientlen;
  struct sockaddr_in clientAddr;
  struct timeval timeout= {.tv_sec = 2, .tv_usec = 0};
  int res;

  clientlen = sizeof(clientAddr);

  while(socketDesc->running)
  {

    FD_ZERO(&socketDesc->readfds); // initialize the fd set
    FD_SET(socketDesc->socketFd, &socketDesc->readfds); // add socket fd

    timeout.tv_sec = 2;
    timeout.tv_usec = 0;

    res = select(socketDesc->socketFd + 1, &socketDesc->readfds, NULL, NULL, &timeout);
    if(res < 0)
    {
      log_err("ERROR in select");
    }

    if(res > 0)
    {
      //process connection requeusts
      if(FD_ISSET(socketDesc->socketFd, &(socketDesc->readfds)))
      {
        //wait for connection requests
        socketChildFd = accept(socketDesc->socketFd, (struct sockaddr *) &clientAddr, &clientlen);
        if(socketChildFd < 0)
          log_err("ERROR on accept");

        if(startClient(socketChildFd, socketDesc) < 0)
          log_err("startClient failed");
      }
    }
  }
  return NULL;
}

/**
 * \brief: opens a socket server
 *
 * \param *socketInit: pointer to the socket init struct
 * \param * socketUserData: pointer to the user data that should be used
 *
 * \return: pointer to the socket descriptor
 */
void *socketServer_open(struct socket_init *socketInit, void *socketUserData)
{
  int optval;
  struct addrinfo hints, *serverinfo, *iter;

  struct socket_desc *socketDesc;


  memset (&hints, 0, sizeof (hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if (getaddrinfo (socketInit->address, socketInit->port, &hints, &serverinfo) != 0)
  {
    log_err("getaddrinfo failed");
    return NULL;
  }

  socketDesc = malloc(sizeof(struct socket_desc));
  if(!socketDesc)
  {
    log_err("malloc failed");
    return NULL;
  }

  socketDesc->socket_onClose = socketInit->socket_onClose;
  socketDesc->socket_onOpen = socketInit->socket_onOpen;
  socketDesc->socket_onMessage = socketInit->socket_onMessage;
  socketDesc->socketUserData = socketUserData;
  socketDesc->list = NULL;
  pthread_mutex_init(&socketDesc->mutex, NULL);

  for(iter = serverinfo; iter != NULL; iter = iter->ai_next)
  {
    if((socketDesc->socketFd = socket(iter->ai_family, iter->ai_socktype, iter->ai_protocol)) < 0)
    {
      log_err("socket failed");
      continue;
    }

    optval = 1;
    if(setsockopt(socketDesc->socketFd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
    {
      log_err("setsockopt SO_REUSEADDR failed");
    }

    optval = 1;
    if(setsockopt(socketDesc->socketFd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
    {
      log_err("setsockopt SO_KEEPALIVE failed");
    }

    optval = 180;
    if(setsockopt(socketDesc->socketFd, IPPROTO_TCP, TCP_KEEPIDLE, &optval, sizeof(optval)) < 0)
    {
      log_err("setsockopt TCP_KEEPIDLE failed");
    }

    optval = 3;
    if(setsockopt(socketDesc->socketFd, IPPROTO_TCP, TCP_KEEPCNT, &optval, sizeof(optval)) < 0)
    {
      log_err("setsockopt TCP_KEEPCNT failed");
    }

    optval = 10;
    if(setsockopt(socketDesc->socketFd, IPPROTO_TCP, TCP_KEEPINTVL, &optval, sizeof(optval)) < 0)
    {
      log_err("setsockopt TCP_KEEPINTVL failed");
    }

    if(bind(socketDesc->socketFd, iter->ai_addr, iter->ai_addrlen) == -1)
    {
      log_err("bind");
      close(socketDesc->socketFd);
      continue;
    }
    break;
  }

  if (iter == NULL)
  {
    log_err("Failed to bind to address and port");
    freeaddrinfo(serverinfo);
    free(socketDesc);
    return NULL;
  }

  freeaddrinfo(serverinfo);

  //wait for connections allow up to 10 connections in queue
  if(listen(socketDesc->socketFd, 10) < 0)
    log_err("listen failed");

  socketDesc->running = true;

  pthread_create(&socketDesc->tid, NULL, socketServerThread, socketDesc);

  return socketDesc;
}

/**
 * \brief: closes the given socket server
 *
 * \param *socketDesc: pointer to the socket descriptor as retrieved from socketServer_open
 *
 */
void socketServer_close(void *socketDesc)
{
  struct socket_desc *sockDesc = socketDesc;

  log_dbg("stopping socket server.\n");
  closeAllClients(sockDesc);
  sockDesc->running = false;
  pthread_join(sockDesc->tid, NULL);
  pthread_mutex_destroy(&sockDesc->mutex);
  close(sockDesc->socketFd);
  free(sockDesc);
}
