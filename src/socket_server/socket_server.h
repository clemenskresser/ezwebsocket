/*
 * socket_server.h
 *
 *  Created on: Mar 23, 2017
 *      Author: Clemens Kresser
 *      License: MIT
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
  size_t (*socket_onMessage)(void *socketUserData, void *connectionDesc, void *clientUserData, void *msg, size_t len);
  void* (*socket_onOpen)(void *socketUserData, struct socket_connection_desc *connectionDesc);
  void (*socket_onClose)(void *socketUserData, void *connectionDesc, void *clientUserData);
  char *port;
  char *address;
};

void socketServer_closeConnection(struct socket_connection_desc *socketClientDesc);
int socketServer_send(struct socket_connection_desc *connectionDesc, void *msg, size_t len);
struct socket_server_desc *socketServer_open(struct socket_server_init *socketInit, void *socketUserData);
void socketServer_close(struct socket_server_desc *socketDesc);


//int socketServer_mainLoop(struct socket_init *socketInit, void *socketUserData);


#endif /* SOCKET_SERVER_H_ */
