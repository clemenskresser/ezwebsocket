# ezwebsocket
Simple Websocket Library written in C

# Features:

* Callbacks for Events (onopen, onclose, onmessage)
* Passes all tests of autobahn test suite
* No external dependencies except pthreads
* reference counting for buffers and descriptors
* MIT License

# Installation:

* download and extract
* $> ./autogen.sh
* $> ./configure --prefix=/usr
* $> make
* $> sudo make install

# Example

compile with:

$> gcc example.c -lezwebsocket -o example

```c
/*
 * example.c
 *
 *  Created on: Mar 22, 2017
 *      Author: Clemens Kresser
 *      License: MIT
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
  printf("%s()\n", __func__);
  if(dataType == WS_DATA_TYPE_TEXT)
  {
    printf("received:%s\n",(char*)msg);
  }
  websocket_sendData(clientDesc, dataType, msg, len);
}

void onClose(void *socketUserData, void *clientDesc, void *userData)
{
  printf("%s()\n", __func__);
}

int main(int argc, char *argv[])
{
  struct websocket_init websocketInit;
  void *wsDesc;

  websocketInit.port = "9001";
  websocketInit.address = "0.0.0.0";
  websocketInit.ws_onOpen = onOpen;
  websocketInit.ws_onClose = onClose;
  websocketInit.ws_onMessage = onMessage;

  wsDesc = websocket_open(&websocketInit, NULL);
  if (wsDesc == NULL)
    return -1;

  for(;;)
  {
    sleep(10);
  }

  websocket_close(wsDesc);

  return 0;
}

```
