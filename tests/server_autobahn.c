/*
 * server_autobahn.c
 *
 *  Created on: Nov 19, 2020
 *      Author: clemens
 */
#include <stdio.h>
#include <unistd.h>
#include <websocket.h>

void *onOpen(void *socketUserData, void *clientDesc)
{
  printf("%s()\n", __func__);

  //you can return user data here which is then
  //passed to onMessage as userData
  return NULL;
}

void onMessage(void *socketUserData, void *clientDesc, void *userData, enum ws_data_type dataType, void *msg, size_t len)
{
  websocket_sendData(clientDesc, dataType, msg, len);
}

void onClose(void *socketUserData, void *clientDesc, void *userData)
{
  printf("%s()\n", __func__);
}

int main(int argc, char *argv[])
{
  struct websocket_server_init websocketInit;
  void *wsDesc;

  websocketInit.port = "9001";
  websocketInit.address = "0.0.0.0";
  websocketInit.ws_onOpen = onOpen;
  websocketInit.ws_onClose = onClose;
  websocketInit.ws_onMessage = onMessage;

  wsDesc = websocketServer_open(&websocketInit, NULL);
  if (wsDesc == NULL)
    return -1;

  for(;;)
  {
    sleep(10);
  }

  websocket_close(wsDesc);

  return 0;
}
