/**
 * \file      socket_server.h
 * \author    Clemens Kresser
 * \date      Mar 23, 2017
 * \copyright Copyright 2017-2020 Clemens Kresser. All rights reserved.
 * \license   This project is released under the MIT License.
 * \brief     Event based socket server implementation
 *
 */

#ifndef SOCKET_SERVER_H_
#define SOCKET_SERVER_H_

#include <stddef.h>

//! prototype for the socket connection descriptor
struct socket_connection_desc;
//! prototype for the socket server descriptor
struct socket_server_desc;

struct socket_server_init
{
  //! callback that is called when data is received
  size_t (*socket_onMessage)(void *socketUserData, void *connectionDesc, void *clientUserData, void *msg, size_t len);
  //! callback that is called when a new connection is established
  void* (*socket_onOpen)(void *socketUserData, struct socket_connection_desc *connectionDesc);
  //! callback that is called when a connection is closed
  void (*socket_onClose)(void *socketUserData, void *connectionDesc, void *clientUserData);
  //! the listening port as string
  char *port;
  //! the listening address as string
  char *address;
};

void socketServer_closeConnection(struct socket_connection_desc *socketClientDesc);
int socketServer_send(struct socket_connection_desc *connectionDesc, void *msg, size_t len);
struct socket_server_desc *socketServer_open(struct socket_server_init *socketInit, void *socketUserData);
void socketServer_close(struct socket_server_desc *socketDesc);


#endif /* SOCKET_SERVER_H_ */
