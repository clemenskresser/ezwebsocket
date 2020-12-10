# ezwebsocket
Simple Websocket Library that supports server and client written in C

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
/**
 * \file      example_server.c
 * \author    Clemens Kresser
 * \date      Mar 22, 2017
 * \copyright Copyright 2017-2020 Clemens Kresser. All rights reserved.
 * \license   This project is released under the MIT License.
 * \brief     simple server example
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <websocket.h>
#include <string.h>

void* onOpen(void *websocketUserData, struct websocket_server_desc *wsDesc,
             struct websocket_connection_desc *connectionDesc)
{
  printf("%s()\n", __func__);

  //you can return user data here which is then
  //passed to onMessage as connectionUserData
  return NULL;
}

void onMessage(void *websocketUserData, struct websocket_connection_desc *connectionDesc, void *connectionUserData,
               enum ws_data_type dataType, void *msg, size_t len)
{
  printf("%s()\n", __func__);
  if(dataType == WS_DATA_TYPE_TEXT)
  {
    printf("received:%s\n", (char*)msg);
  }
  // echo the received data
  websocket_sendData(connectionDesc, dataType, msg, len);
}

void onClose(struct websocket_server_desc *wsDesc, void *websocketUserData,
             struct websocket_connection_desc *connectionDesc, void *connectionUserData)
{
  printf("%s()\n", __func__);
}

int main(int argc, char *argv[])
{
  struct websocket_server_init websocketInit;
  struct websocket_server_desc *wsServerDesc;

  websocketInit.port = "9001";
  websocketInit.address = "0.0.0.0";
  websocketInit.ws_onOpen = onOpen;
  websocketInit.ws_onClose = onClose;
  websocketInit.ws_onMessage = onMessage;

  wsServerDesc = websocketServer_open(&websocketInit, NULL);
  if(wsServerDesc == NULL)
    return -1;

  for(;;)
  {
    sleep(1);
  }

  websocketServer_close(wsServerDesc);

  return 0;
}
```

# Websocket client example

compile with:

$> gcc example_client.c -lezwebsocket -o example_client

```c
/**
 * \file      example_client.c
 * \author    Clemens Kresser
 * \date      Nov 17, 2020
 * \copyright Copyright 2017-2020 Clemens Kresser. All rights reserved.
 * \license   This project is released under the MIT License.
 * \brief     simple client example
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <websocket.h>
#include <string.h>

void* onOpen(void *socketUserData, struct websocket_client_desc *wsDesc,
             struct websocket_connection_desc *connectionDesc)
{
  printf("%s()\n", __func__);

  //you can return user data here which is then
  //passed to onMessage as userData
  return NULL;
}

void onMessage(void *socketUserData, struct websocket_connection_desc *connectionDesc, void *connectionUserData,
               enum ws_data_type dataType, void *msg, size_t len)
{
  if(dataType == WS_DATA_TYPE_TEXT)
  {
    printf("received:%s\n", (char*)msg);
  }
}

void onClose(void *socketUserData, struct websocket_connection_desc *connectionDesc, void *connectionUserData)
{
  printf("%s()\n", __func__);
}

int main(int argc, char *argv[])
{
  struct websocket_client_init websocketInit;
  struct websocket_connection_desc *wsConnectionDesc;

  const char *sendText = "Hello World From Ezwebsocket";

  websocketInit.port = "9001";
  websocketInit.address = "127.0.0.1";
  websocketInit.ws_onOpen = onOpen;
  websocketInit.ws_onClose = onClose;
  websocketInit.ws_onMessage = onMessage;

  wsConnectionDesc = websocketClient_open(&websocketInit, NULL);
  if(wsConnectionDesc == NULL)
    return -1;

  for(unsigned long i = 0; i < 10; i++)
  {
    //send "Hello World From Ezwebsocket" every second
    websocket_sendData(wsConnectionDesc, WS_DATA_TYPE_TEXT, sendText, strlen(sendText) + 1);
    sleep(1);
  }

  websocketClient_close(wsConnectionDesc);

  return 0;
}
```

# Running the Autobahn Tests

## Prerequisites

Download the Autobahn docker image from:

<https://hub.docker.com/r/crossbario/autobahn-testsuite/tags/>

## Websocket client Test

Create a directory for the binary:

$> mkdir -p tests/bin

Compile test with:

$> gcc tests/client_autobahn.c -lezwebsocket -o tests/bin/client_autobahn

Start autobahn docker image with:

$> cd tests && ./run_autobahn_client_test.sh

Start websocket test program with:

$> ./tests/bin/client_autobahn

You will then find the report at tests/reports/clients/index.html

## Websocket Server Test

Create a directory for the binary:

$> mkdir -p tests/bin

Compile test with:

$> gcc tests/server_autobahn.c -lezwebsocket -o tests/bin/server_autobahn

Start websocket test program with:

$> ./tests/bin/server_autobahn

Start autobahn docker image with:

$> cd tests && ./run_autobahn_client_test.sh

You will then find the report at tests/reports/clients/index.html


