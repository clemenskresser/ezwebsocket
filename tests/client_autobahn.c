/*
 * client_autobahn.c
 *
 *  Created on: Nov 19, 2020
 *      Author: Clemens Kresser
 *      License: MIT
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "websocket.h"
#include <unistd.h>

static uint32_t numberOfTestCases = 0;
static uint32_t currentTestNum = 0;

void* onOpen(void *socketUserData, struct websocket_connection_desc *connectionDesc)
{
  //we just allocate some bytes here to see if we don't have memory leaks
  return malloc(0xDEAD);
}

void onMessage(void *socketUserData, struct websocket_connection_desc *connectionDesc, void *userData,
               enum ws_data_type dataType, void *msg, size_t len)
{
  if((currentTestNum == 0) && (dataType == WS_DATA_TYPE_TEXT))
  {
    for(uint32_t i = 0; i < len; i++)
    {
      numberOfTestCases *= 10;
      numberOfTestCases = numberOfTestCases + ((char*)msg)[i] - '0';
    }
    printf("numberOfTestCases=%u\n", numberOfTestCases);
  }
  else
    websocket_sendData(connectionDesc, dataType, msg, len);
}

void onClose(void *socketUserData, struct websocket_connection_desc *connectionDesc, void *userData)
{
  free(userData);
}

/**
 * \brief This runs the test as required by the autobahn testsuite
 */
void runTest(void)
{
  struct websocket_client_init websocketInit;
  struct websocket_connection_desc *wsConnectionDesc;

  websocketInit.port = "9001";
  websocketInit.address = "127.0.0.1";
  websocketInit.hostname = "arc";
  websocketInit.ws_onOpen = onOpen;
  websocketInit.ws_onClose = onClose;
  websocketInit.ws_onMessage = onMessage;

  do
  {
    printf("currentTestNum=%u\n", currentTestNum);
    if(currentTestNum == 0)
    {
      websocketInit.endpoint = strdup("/getCaseCount");
    }
    else
    {
      free((char*)websocketInit.endpoint);
      asprintf((char**)&websocketInit.endpoint, "/runCase?case=%u&agent=EZwebsocket", currentTestNum);
    }

    wsConnectionDesc = websocketClient_open(&websocketInit, NULL);

    while(websocketConnection_isConnected(wsConnectionDesc))
      usleep(100000);

    websocketClient_close(wsConnectionDesc);

    currentTestNum++;

  }while(currentTestNum <= numberOfTestCases);
  free((char*)websocketInit.endpoint);

  websocketInit.endpoint = "/updateReports?agent=EZwebsocket";

  wsConnectionDesc = websocketClient_open(&websocketInit, NULL);

  while(websocketConnection_isConnected(wsConnectionDesc))
    usleep(100000);

  websocketClient_close(wsConnectionDesc);

  currentTestNum = 0;
  numberOfTestCases = 0;
}

/**
 * \brief The main function
 */
int main(int argc, char *argv[])
{
  runTest();
}
