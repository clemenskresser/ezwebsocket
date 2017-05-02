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

struct socket_init
{
  size_t (*socket_onMessage)(void *socketUserData, void *clientDesc, void *clientUserData, void *msg, size_t len);
  void* (*socket_onOpen)(void *socketUserData, void *clientDesc);
  void (*socket_onClose)(void *socketUserData, void *clientDesc, void *clientUserData);
  char *port;
  char *address;
};

void socketServer_closeClient(void *socketClientDesc);
int socketServer_send(void *clientDesc, void *msg, size_t len);
void *socketServer_open(struct socket_init *socketInit, void *socketUserData);
void socketServer_close(void *socketDesc);


//int socketServer_mainLoop(struct socket_init *socketInit, void *socketUserData);


#endif /* SOCKET_SERVER_H_ */
