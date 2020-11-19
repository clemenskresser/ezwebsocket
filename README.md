# ezwebsocket
Simple Websocket Library written in C

# Features:

* Websocket server
* Websocket client
* Simple usage
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

# Websocket server example

compile with:

$> gcc example_server.c -lezwebsocket -o example_server

```c
/*
 * example_server.c
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

```

# Websocket client example

compile with:

$> gcc example_client.c -lezwebsocket -o example_client

```c
/*
 * example_client.c
 *
 *  Created on: Nov 17, 2020
 *      Author: Clemens Kresser
 *      License: MIT
 */

#include <stdio.h>
#include <unistd.h>
#include <websocket.h>
#include <string.h>

void *onOpen(void *socketUserData, void *wsDesc, void *clientDesc)
{
  printf("%s()\n", __func__);

  //you can return user data here which is then
  //passed to onMessage as userData
  return NULL;
}
static int count = 0;

void onMessage(void *socketUserData, void *sessionDesc, void *sessionUserData, enum ws_data_type dataType, void *msg, size_t len)
{
  printf("%s() %u\n", __func__, count++);
  if(dataType == WS_DATA_TYPE_TEXT)
  {
    printf("received:%s\n",(char*)msg);
  }
}

void onClose(void *socketUserData, void *clientDesc, void *userData)
{
  printf("%s() %u\n", __func__, count);
}

int main(int argc, char *argv[])
{
  struct websocket_client_init websocketInit;
  void *wsDesc;

  const char *sendText = "Hello World From Ezwebsocket";

  websocketInit.port = "9001";
  websocketInit.address = "127.0.0.1";
  websocketInit.ws_onOpen = onOpen;
  websocketInit.ws_onClose = onClose;
  websocketInit.ws_onMessage = onMessage;

  wsDesc = websocketClient_open(&websocketInit, NULL);
  if (wsDesc == NULL)
    return -1;

  for(unsigned long i = 0; i < 10; i++)
  {
	websocket_sendData(wsDesc, WS_DATA_TYPE_TEXT, sendText, strlen(sendText) + 1);
    sleep(1);
  }

  websocketClient_close(wsDesc);

  return 0;
}

```

# Running the Autobahn Tests

## Prerequisites

Download the Autobahn docker image from:

<https://hub.docker.com/r/crossbario/autobahn-testsuite/tags/>

## Websocket client Test

Create a directory for the binary:

mkdir -p tests/bin

Compile test with:

$> gcc tests/client_autobahn.c -lezwebsocket -o tests/bin/client_autobahn

Start autobahn docker image with:

$> cd tests && ./run_autobahn_client_test.sh

Start websocket test program with:

$> ./tests/bin/client_autobahn

You will then find the report at tests/reports/clients/index.html

## Websocket Server Test

Create a directory for the binary:

mkdir -p tests/bin

Compile test with:

$> gcc tests/server_autobahn.c -lezwebsocket -o tests/bin/server_autobahn

Start websocket test program with:

$> ./tests/bin/server_autobahn

Start autobahn docker image with:

$> cd tests && ./run_autobahn_client_test.sh

You will then find the report at tests/reports/clients/index.html


