/*
 * websocket.h
 *
 *  Created on: Mar 24, 2017
 *      Author: Clemens Kresser
 *      License: MIT
 */

#ifndef WEBSOCKET_H_
#define WEBSOCKET_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

enum ws_data_type
{
  WS_DATA_TYPE_TEXT,
  WS_DATA_TYPE_BINARY,
};

enum ws_close_code
{
  WS_CLOSE_CODE_NORMAL = 1000,
  WS_CLOSE_CODE_GOING_AWAY = 1001,
  WS_CLOSE_CODE_PROTOCOL_ERROR = 1002,
  WS_CLOSE_CODE_UNACCEPTABLE_OPCODE = 1003,
  WS_CLOSE_CODE_RESERVED_0 = 1004,
  WS_CLOSE_CODE_RESERVED_1 = 1005,
  WS_CLOSE_CODE_RESERVED_2 = 1006,
  WS_CLOSE_CODE_INVALID_DATA = 1007,
  WS_CLOSE_CODE_POLICY_VIOLATION = 1008,
  WS_CLOSE_CODE_MSG_TO_BIG = 1009,
  WS_CLOSE_CODE_CLIENT_EXTENSION_UNKNOWN = 1010,
  WS_CLOSE_CODE_UNEXPECTED_COND = 1011,
  WS_CLOSE_CODE_RESERVED_3 = 1015,
};

struct websocket_init
{
  void (*ws_onmessage)(void *wsDesc, void *clientDesc, void *userData, enum ws_data_type dataType, void *msg, size_t len);
  void* (*ws_onOpen)(void *wsDesc, void *clientDesc);
  void (*ws_onClose)(void *wsDesc, void *clientDesc, void *userData);
  char *address;
  char *port;
};

/**
 * \brief: returns the user data of the given client
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 *
 */
void *websocket_getClientUserData(void *wsClientDesc);

/**
 * \brief: opens a websocket server
 *
 * \param wsInit: pointer to the init struct
 * \param userData: userData for the socket
 *
 * \return: the websocket descriptor or NULL in case of error
 */
void *websocket_open(struct websocket_init *wsInit, void *userData);

/**
 * \brief: closes the given websocket
 *
 * \param *wsDesc: pointer to the websocket descriptor
 *
 */
void websocket_close(void *wsDesc);

/**
 * \brief: increments the reference count of the given object
 *
 * \param *ptr: poiner to the object
 */
void websocket_ref(void *ptr);

/**
 * \brief: decrements the reference count of the given object and frees it if necessary
 *
 * \param *ptr: poiner to the object
 */
void websocket_unref(void *ptr);

/**
 * \brief: sends binary or text data through websockets
 *
 * \param *wsClientDesc: pointer to the client descriptor
 * \param dataType: the datatype (WS_DATA_TYPE_BINARY or WS_DATA_TYPE_TEXT)
 * \param *msg: the payload data
 * \param len: the payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendData(void *wsClientDesc, enum ws_data_type dataType, void *msg, size_t len);

/**
 * \brief: closes the given websocket client
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param code: the closing code
 */
void websocket_closeClient(void *wsClientDesc, enum ws_close_code code);

/**
 * \brief: sends fragmented binary or text data through websockets
 *         use websocket_sendDataFragmetedCont for further fragments
 *
 * \param *wsClientDesc: pointer to the client descriptor
 * \param dataType: the datatype (WS_DATA_TYPE_BINARY or WS_DATA_TYPE_TEXT)
 * \param *msg: the payload data
 * \param len: the payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendDataFragmentedStart(void *wsClientDesc, enum ws_data_type dataType, void *msg, size_t len);

/**
 * \brief: continues a fragmented send
 *         use sendDataFragmentedStop to stop the transmission
 *
 * \param *wsClientDesc: pointer to the client descriptor
 * \param fin: true => this is the last fragment else false
 * \param *msg: the payload data
 * \param len: the payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendDataFragmentedCont(void *wsClientDesc, bool fin, void *msg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_H_ */
