/*
 * socket_client.h
 *
 *  Created on: Jun 26, 2020
 *      Author: Clemens Kresser
 *      License: MIT
 */
#ifndef SOCKET_CLIENT_SOCKET_CLIENT_H_
#define SOCKET_CLIENT_SOCKET_CLIENT_H_

#include <stddef.h>
#include "utils/dyn_buffer.h"

struct socket_client_init
{
  //! callback that should be called when a message is received use NULL if not used
  size_t (*socket_onMessage)(void *socketUserData, void *socketDesc, void *sessionData, void *msg, size_t len);
  //! callback that should be called when the socket is connected use NULL if not used
  void* (*socket_onOpen)(void *socketUserData, void *socketDesc);
  //! callback that should be called when the socket is closed use NULL if not used
  void (*socket_onClose)(void *socketUserData, void *socketDesc, void *sessionData);
  //! the remote port we want to connect to
  unsigned short port;
  //! the address we want to connect to
  const char *address;
};

int socketClient_send(void *socketDescriptor, void *msg, size_t len);
void socketClient_start(void *socketDescriptor);
void *socketClient_open(struct socket_client_init *socketInit, void *socketUserData);
void socketClient_close(void *socketDescriptor);
void socketClient_closeConnection(void *socketDescriptor);

#endif /* SOCKET_CLIENT_SOCKET_CLIENT_H_ */
