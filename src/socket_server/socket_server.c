/**
 * \file      socket_server.c
 * \author    Clemens Kresser
 * \date      Mar 23, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     Event based socket server implementation
 *
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


//! starting size of the message buffer (will be increased everytime the buffer is to small)
#define READ_SIZE 1024

//! States of the socket connection
enum socket_connection_state
{
  //! socket connected state
  SOCKET_SESSION_STATE_CONNECTED,
  //! socket disconnected state
  SOCKET_SESSION_STATE_DISCONNECTED,
};

//! structure that holds information about the connection
struct socket_connection_desc
{
  //! the connection state
  volatile enum socket_connection_state state;
  //! file descriptor for the connection
  int connectionSocketFd;
  //! pointer to the socket descriptor
  struct socket_server_desc *socketDesc;
  //! the thread id of the connectionThread
  pthread_t tid;
  //! the buffer for the data that was received
  struct dyn_buffer buffer;
  //! the user data for the connection
  void *connectionUserData;
};

//! structure needed for the linked list that contains all connections
struct socket_connection_list_element
{
  //! descriptor of the connection
  struct socket_connection_desc *desc;
  //! the next element in the list
  struct socket_connection_list_element *next;
};

//! structure that stores all data of a socket server
struct socket_server_desc
{
  //! list that holds all connections
  struct socket_connection_list_element *list;
  //! mutex that protects access to the list
  pthread_mutex_t listMutex;
  //! function that should be called when data is received
  size_t (*socket_onMessage)(void *socketUserData, void *connectionDesc, void *connectionUserData, void *msg, size_t len);
  //! function that should be called when a connection is established
  void* (*socket_onOpen)(void *socketUserData, struct socket_connection_desc *connectionDesc);
  //! function that should be called when a connection is closed
  void (*socket_onClose)(void *socketUserData, void *connectionDesc, void *connectionUserData);
  //! user data for the server socket
  void *socketUserData;
  //! file descriptor of the socket
  int socketFd;
  //! file descriptor set for read
  fd_set readfds;
  //! indicates if the socket is still running
  volatile bool running;
  //! the thread ID of the socketServerThread
  pthread_t tid;
  //! the current number of connections
  unsigned long numConnections;
};

/**
 * \brief Adds a connection to the list of the socket descriptor
 *
 * \param *socketDesc Pointer to the socket descriptor
 * \param *desc Pointer to the connection descriptor that should be added
 *
 * \return 0 if successful else -1
 */
static int addConnection(struct socket_server_desc *socketDesc, struct socket_connection_desc *desc)
{
  struct socket_connection_list_element *listElement;

  listElement = malloc(sizeof(struct socket_connection_list_element));
  if(!listElement)
  {
    log_err("malloc failed");
    return -1;
  }

  refcnt_ref(desc);
  listElement->desc = desc;

  pthread_mutex_lock(&socketDesc->listMutex);
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
  pthread_mutex_unlock(&socketDesc->listMutex);

  return 0;
}

/**
 * \brief removes a connection from the list of the socket descriptor
 *
 * \param *socketDesc Pointer to the socket descriptor
 * \param *desc Pointer to the connection descriptor that should be removed
 *
 * \return 0 if successful else -1
 */
static int removeConnection(struct socket_server_desc *socketDesc, struct socket_connection_desc *desc)
{
  struct socket_connection_list_element *listElement, *previousListElement = NULL;
  int rc = -1;

  pthread_mutex_lock(&socketDesc->listMutex);
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
  pthread_mutex_unlock(&socketDesc->listMutex);
  refcnt_unref(desc);

  return rc;
}

/**
 * \brief closes all connections
 *
 * \param *socketDesc Pointer to the socket descriptor
 */
static void closeAllConnections(struct socket_server_desc *socketDesc)
{
  struct socket_connection_list_element *listElement;

  pthread_mutex_lock(&socketDesc->listMutex);
  {
    listElement = socketDesc->list;
    while(listElement)
    {
      listElement = listElement->next;
      socketServer_closeConnection(listElement->desc);
    }
  }
  pthread_mutex_unlock(&socketDesc->listMutex);
}


/**
 * \brief connection thread
 *
 * \param *params Pointer to the connectionDesc (steals the reference)
 *
 * \return NULL
 */
static void *connectionThread(void *params)
{
  struct socket_connection_desc *connectionDesc = params;
  fd_set readfds;
  int n;
  size_t count;
  int increase;
  size_t bytesFree;
  bool first;
  struct timeval tv;

  pthread_detach(pthread_self());

  connectionDesc->connectionUserData = connectionDesc->socketDesc->socket_onOpen(connectionDesc->socketDesc->socketUserData, connectionDesc);

  FD_ZERO(&readfds);
  FD_SET(connectionDesc->connectionSocketFd, &readfds);
  while(connectionDesc->state == SOCKET_SESSION_STATE_CONNECTED)
  {
    tv.tv_sec = 0;
    tv.tv_usec = 300000;
    FD_ZERO(&readfds);
    FD_SET(connectionDesc->connectionSocketFd, &readfds);
    if(select(connectionDesc->connectionSocketFd + 1, &readfds, NULL, NULL, &tv) > 0)
    {
      if(FD_ISSET(connectionDesc->connectionSocketFd, &readfds))
      {
        first = true;
        increase = 1;
        do
        {
          bytesFree = DYNBUFFER_BYTES_FREE(&connectionDesc->buffer);
          if(DYNBUFFER_BYTES_FREE(&connectionDesc->buffer) < READ_SIZE)
          {
            dynBuffer_increase_to(&(connectionDesc->buffer), READ_SIZE * increase);
            bytesFree = DYNBUFFER_BYTES_FREE(&connectionDesc->buffer);
            increase++;
          }
          n = recv(connectionDesc->connectionSocketFd, DYNBUFFER_WRITE_POS(&(connectionDesc->buffer)), bytesFree, MSG_DONTWAIT);
          if(first && (n == 0))
          {
            connectionDesc->state = SOCKET_SESSION_STATE_DISCONNECTED;
            break;
          }
          first = false;

          if(n >= 0)
            DYNBUFFER_INCREASE_WRITE_POS((&(connectionDesc->buffer)), n);
          else
            break;
        } while(((size_t)n == bytesFree) && (connectionDesc->state == SOCKET_SESSION_STATE_CONNECTED));

        if(connectionDesc->state == SOCKET_SESSION_STATE_CONNECTED)
        {
          do
          {
            count = connectionDesc->socketDesc->socket_onMessage(connectionDesc->socketDesc->socketUserData, connectionDesc, connectionDesc->connectionUserData,
                DYNBUFFER_BUFFER(&(connectionDesc->buffer)), DYNBUFFER_SIZE(&(connectionDesc->buffer)));
            dynBuffer_removeLeadingBytes(&(connectionDesc->buffer), count);
          } while(count && DYNBUFFER_SIZE(&(connectionDesc->buffer)) && (connectionDesc->state == SOCKET_SESSION_STATE_CONNECTED));
        }
      }
    }
  }

  dynBuffer_delete(&(connectionDesc->buffer));

  connectionDesc->state = SOCKET_SESSION_STATE_DISCONNECTED;
  connectionDesc->socketDesc->socket_onClose(connectionDesc->socketDesc->socketUserData, connectionDesc, connectionDesc->connectionUserData);
  close(connectionDesc->connectionSocketFd);
  removeConnection(connectionDesc->socketDesc, connectionDesc);

  refcnt_unref(connectionDesc);

  return NULL;
}

/**
 * \brief starts a new connection
 *
 * \param socketFd The socket file descriptor
 * \param socketDesc Pointer to the socket descriptor
 *
 * \return 0 if successful else -1
 */
static int startConnection(int socketFd, struct socket_server_desc *socketDesc)
{
  struct socket_connection_desc *desc;

  desc = refcnt_allocate(sizeof(struct socket_connection_desc), NULL);
  if(!desc)
  {
    log_err("refcnt_allocate failed");
    return -1;
  }

  memset(desc, 0, sizeof(struct socket_connection_desc));

  desc->connectionSocketFd = socketFd;
  desc->socketDesc = socketDesc;
  dynBuffer_init(&(desc->buffer));
  desc->connectionUserData = NULL;

  addConnection(socketDesc, desc);
  desc->state = SOCKET_SESSION_STATE_CONNECTED;

  if(pthread_create(&desc->tid, NULL, connectionThread, desc) != 0)
  {
    log_err("pthread_create failed");
    refcnt_unref(desc);
    return -1;
  }

  return 0;
}

/**
 * \brief closes the given connection
 *
 * \param *socketConnectionDesc Pointer to the socket connection descriptor
 */
void socketServer_closeConnection(struct socket_connection_desc *socketConnectionDesc)
{
  refcnt_ref(socketConnectionDesc);
  socketConnectionDesc->state = SOCKET_SESSION_STATE_DISCONNECTED;
  refcnt_unref(socketConnectionDesc);
}

/**
 * \brief sends the given data over the given socket
 *
 * \param *connectionDesc Pointer to the connection descriptor
 * \param *msg Pointer to the data
 * \param len The length of the data
 *
 * \return 0 if successful else -1
 */
int socketServer_send(struct socket_connection_desc *connectionDesc, void *msg, size_t len)
{
  int rc;
  if(connectionDesc->state == SOCKET_SESSION_STATE_DISCONNECTED)
    return -1;

  rc = send(connectionDesc->connectionSocketFd, msg, len, MSG_NOSIGNAL);
  if(rc == -1)
  {
    log_err("send failed: %s", strerror(errno));
  }
  return ((size_t)rc == len ? 0 : -1);
}

/**
 * \brief processes connection requests
 *
 * \param *sockDesc Pointer to the socket descriptor
 *
 * \return NULL
 *
 */
static void *socketServerThread(void *sockDesc)
{
  struct socket_server_desc *socketDesc = sockDesc;
  int socketChildFd;
  socklen_t connectionAddrLen;
  struct sockaddr_in connectionAddr;
  struct timeval timeout= {.tv_sec = 2, .tv_usec = 0};
  int res;

  connectionAddrLen = sizeof(connectionAddr);

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
        socketChildFd = accept(socketDesc->socketFd, (struct sockaddr *) &connectionAddr, &connectionAddrLen);
        if(socketChildFd < 0)
          log_err("ERROR on accept");

        if(startConnection(socketChildFd, socketDesc) < 0)
          log_err("startConnection failed");
      }
    }
  }
  return NULL;
}

/**
 * \brief opens a socket server
 *
 * \param *socketInit Pointer to the socket init struct
 * \param *socketUserData Pointer to the user data that should be used
 *
 * \return pointer to the socket descriptor
 */
struct socket_server_desc *socketServer_open(struct socket_server_init *socketInit, void *socketUserData)
{
  int optval;
  struct addrinfo hints, *serverinfo, *iter;
  struct socket_server_desc *socketDesc;

  memset (&hints, 0, sizeof (hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if(getaddrinfo(socketInit->address, socketInit->port, &hints, &serverinfo) != 0)
  {
    log_err("getaddrinfo failed");
    return NULL;
  }

  socketDesc = malloc(sizeof(struct socket_server_desc));
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
  socketDesc->numConnections = 0;
  pthread_mutex_init(&socketDesc->listMutex, NULL);

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
 * \brief closes the given socket server
 *
 * \param *socketDesc Pointer to the socket descriptor as retrieved from socketServer_open
 *
 */
void socketServer_close(struct socket_server_desc *socketDesc)
{
  if(socketDesc == NULL)
      return;

  log_dbg("stopping socket server.\n");
  closeAllConnections(socketDesc);
  socketDesc->running = false;
  pthread_join(socketDesc->tid, NULL);
  while(socketDesc->numConnections > 0)
      usleep(300000);
  pthread_mutex_destroy(&socketDesc->listMutex);
  close(socketDesc->socketFd);
  free(socketDesc);
}
