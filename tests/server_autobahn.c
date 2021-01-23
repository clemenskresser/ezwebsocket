/*
 * server_autobahn_legacy.c
 *
 *  Created on: Nov 19, 2020
 *      Author: Clemens Kresser
 *     License: MIT
 */
#include <stdio.h>
#include <unistd.h>
#include <websocket.h>
#include <stdlib.h>
#include <signal.h>

bool debugEnabled = false;
static bool stop = false;

void sigIntHandler(int dummy)
{
  stop = true;
}

void* onOpen(void *socketUserData, struct websocket_server_desc *wsDesc,
             struct websocket_connection_desc *connectionDesc)
{
  //we just allocate some bytes here to see if we don't have memory leaks
  return malloc(0xDEAD);
}

void onMessage(void *socketUserData, struct websocket_connection_desc *connectionDesc, void *userData,
               enum ws_data_type dataType, void *msg, size_t len)
{
  websocket_sendData(connectionDesc, dataType, msg, len);
}

void onClose(struct websocket_server_desc *wsDesc, void *socketUserData,
             struct websocket_connection_desc *connectionDesc, void *userData)
{
  free(userData);
}

int main(int argc, char *argv[])
{
  struct websocket_server_init websocketInit;
  struct websocket_server_desc *wsDesc;

  signal(SIGINT, sigIntHandler);

  websocketInit.port = "9001";
  websocketInit.address = "0.0.0.0";
  websocketInit.ws_onOpen = onOpen;
  websocketInit.ws_onClose = onClose;
  websocketInit.ws_onMessage = onMessage;

  wsDesc = websocketServer_open(&websocketInit, NULL);
  if(wsDesc == NULL)
    return -1;

  while(!stop)
  {
    usleep(300000);
  }

  websocketServer_close(wsDesc);

  return 0;
}
