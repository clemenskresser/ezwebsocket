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

//! NOTE  this define is for backward compatibility
//!       it should not be used anymore but will not be
//!       removed unless there's a good reason
struct websocket_init
{
  //! callback that is called when a message is received
  void (*ws_onMessage)(void *websocketUserData, void *clientDesc, void *clientUserData,
          enum ws_data_type dataType, void *msg, size_t len);
  //! callback that is called when a new connection is established
  void* (*ws_onOpen)(void *wsDesc, void *clientDesc);
  //! callback that is called when a connection is closed
  void (*ws_onClose)(void *wsDesc, void *clientDesc, void *userData);
  //! the listening address
  char *address;
  //! the listening port
  char *port;
};

struct websocket_server_init
{
  //! callback that is called when a message is received
  void (*ws_onMessage)(void *websocketUserData, void *clientDesc, void *clientUserData,
          enum ws_data_type dataType, void *msg, size_t len);
  //! callback that is called when a new connection is established
  void* (*ws_onOpen)(void *wsDesc, void *clientDesc);
  //! callback that is called when a connection is closed
  void (*ws_onClose)(void *wsDesc, void *clientDesc, void *userData);
  //! the listening address
  char *address;
  //! the listening port
  char *port;
};

struct websocket_client_init
{
  //! callback that is called when a message is received
  void (*ws_onMessage)(void *socketUserData, void *sessionDesc, void *sessionUserData,
		  enum ws_data_type dataType, void *msg, size_t len);
  //! callback that is called when a new connection is established
  void* (*ws_onOpen)(void *socketUserData, void *wsDesc, void *sessionDesc);
  //! callback that is called when a connection is closed
  void (*ws_onClose)(void *socketUserData, void *sessionDesc, void *sessionUserData);
  //! the address of the remote target
  const char *address;
  //! the port of the remote target
  const char *port;
  //! the hostname that should be used
  const char *hostname;
  //! the endpoint of the remote (e.g. /chat)
  const char *endpoint;
};

/**
 * \brief: returns the user data of the given client
 *
 * \param *wsSessionDesc: pointer to the websocket client descriptor
 *
 */
void *websocket_getClientUserData(void *wsSessionDesc);


/**
 * \brief: opens a websocket server
 *
 * \param wsInit: pointer to the init struct
 * \param userData: userData for the socket
 *
 * \return: the websocket descriptor or NULL in case of error
 */
void *websocketServer_open(struct websocket_server_init *wsInit, void *userData);

/**
 * \brief opens a websocket client connection
 *
 * \param wsInit: pointer to the init struct
 * \param websocketUserData: userData for the socket
 *
 * \return: the websocket client descriptor or NULL in case of error
 */
void *websocketClient_open(struct websocket_client_init *wsInit, void *websocketUserData);

/**
 * \brief: closes the given websocket
 *
 * \param *wsDesc: pointer to the websocket descriptor
 *
 */
void websocketServer_close(void *wsDesc);


/**
 * \brief closes a websocket client
 *
 * \param *wsClientDesc Pointer to the websocket client descriptor
 */
void websocketClient_close(void *wsSessionDescriptor);

/**
 * \brief returns if the client is still connected
 *
 * \return true => connected else false
 */
bool websocketClient_isConnected(void *wsSessionDescriptor);

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
 * \param *wsSessionDescriptor: pointer to the websocket session descriptor
 * \param dataType: the datatype (WS_DATA_TYPE_BINARY or WS_DATA_TYPE_TEXT)
 * \param *msg: the payload data
 * \param len: the payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendData(void *wsSessionDescriptor, enum ws_data_type dataType, const void *msg, size_t len);

/**
 * \brief: closes the given websocket session
 *
 * \param *wsSessionDesc: pointer to the websocket session descriptor
 * \param code: the closing code
 */
void websocket_closeSession(void *wsSessionDescriptor, enum ws_close_code code);

/**
 * \brief: sends fragmented binary or text data through websockets
 *         use websocket_sendDataFragmetedCont for further fragments
 *
 * \param *wsSessionDescriptor: pointer to the websocket session descriptor
 * \param dataType: the datatype (WS_DATA_TYPE_BINARY or WS_DATA_TYPE_TEXT)
 * \param *msg: the payload data
 * \param len: the payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendDataFragmentedStart(void *wsSessionDescriptor, enum ws_data_type dataType, const void *msg, size_t len);

/**
 * \brief: continues a fragmented send
 *         use sendDataFragmentedStop to stop the transmission
 *
 * \param *wsSessionDesc: pointer to the websocket session descriptor
 * \param fin: true => this is the last fragment else false
 * \param *msg: the payload data
 * \param len: the payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendDataFragmentedCont(void *wsSessionDescriptor, bool fin, const void *msg, size_t len);


/**
 * \brief: closes the given websocket server
 *
 * \param *wsDesc: pointer to the websocket descriptor
 *
 * \note this define is for backward compatibility
 *       it should not be used anymore but will not be
 *       removed unless there's a good reason
 */
#define websocket_close(wsDesc) websocketServer_close(wsDesc)

/**
 * \brief: opens a websocket server
 *
 * \param wsInit: pointer to the init struct
 * \param userData: userData for the socket
 *
 * \return: the websocket descriptor or NULL in case of error
 *
 * \note this define is for backward compatibility
 *       it should not be used anymore but will not be
 *       removed unless there's a good reason
 */
#define websocket_open(wsInit, userData) websocketServer_open(wsInit, userData)

#ifdef __cplusplus
}
#endif

#endif /* WEBSOCKET_H_ */
