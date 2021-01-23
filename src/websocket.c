/**
 * \file      websocket.c
 * \author    Clemens Kresser
 * \date      Mar 23, 2017
 * \copyright Copyright 2017-2021 Clemens Kresser MIT License
 * \brief     Handles the websocket specific stuff
 *
 */

#define _GNU_SOURCE

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "socket_server/socket_server.h"
#include "socket_client/socket_client.h"
#include "websocket.h"
#include <ctype.h>
#include <string.h>
#include "utils/sha1.h"
#include "utils/stringck.h"
#include "utils/ref_count.h"
#include <time.h>
#include "utils/utf8.h"
#include "utils/log.h"
#include "utils/base64.h"
#include <unistd.h>
#include <limits.h>

#define MESSAGE_TIMEOUT_S 30

#undef VERBOSE_MODE

#define MAX_DEFAULT_PAYLOAD_LENGTH 125
#define EXTENDED_16BIT_PAYLOAD_LENGTH 126
#define EXTENDED_64BIT_PAYLOAD_LENGTH 127

enum ws_state
{
  WS_STATE_HANDSHAKE,
  WS_STATE_CONNECTED,
  WS_STATE_CLOSED
};

enum ws_opcode
{
  WS_OPCODE_CONTINUATION = 0x00,
  WS_OPCODE_TEXT = 0x01,
  WS_OPCODE_BINARY = 0x02,
  WS_OPCODE_DISCONNECT = 0x08,
  WS_OPCODE_PING = 0x09,
  WS_OPCODE_PONG = 0x0A,
};

enum ws_type
{
    WS_TYPE_CLIENT,
    WS_TYPE_SERVER
};

struct websocket_server_desc
{
  //! callback that is called when a message is received on the websocket
  void (*ws_onMessage)(void *websocketUserData, struct websocket_connection_desc *connectionDesc, void *connectionUserData,
                       enum ws_data_type dataType, void *msg, size_t len);
  //! callback that is called when the websocket is connected (legacy version)
  void* (*ws_onOpenLegacy)(struct websocket_server_desc *wsDesc, struct websocket_connection_desc *connectionDesc);
  //! callback that is called when the websocket is connected
  void* (*ws_onOpen)(void *websocketUserData, struct websocket_server_desc *wsDesc, struct websocket_connection_desc *connectionDesc);
  //! callback that is called when the websocket is closed (legacy version)
  void (*ws_onCloseLegacy)(void *websocketUserData, struct websocket_connection_desc *connectionDesc, void *connectionUserData);
  //! callback that is called when the websocket is closed
  void (*ws_onClose)(struct websocket_server_desc *wsDesc, void *websocketUserData, struct websocket_connection_desc *connectionDesc,
                     void *userData);
  //! pointer to the socket descriptor
  void *socketDesc;
  //! pointer to the user data
  void *wsSocketUserData;
};

struct last_message
{
  //! the type of data (WS_DATA_TYPE_TEXT or WS_DATA_TYPE_BINARY)
  enum ws_data_type dataType;
  //! indicates if the first message was received
  bool firstReceived;
  //! indicates if the message is complete
  bool complete;
  //! handle that is used to store the state if using fragmented strings
  unsigned long utf8Handle;
  //! the length of the message
  size_t len;
  //! pointer to the data
  char *data;
};

struct websocket_connection_desc
{
  //! indicates if it is a websocket client or a websocket server
  enum ws_type wsType;
  //! pointer to the socket client descriptor
  void *socketClientDesc;
  //! the connection state of the websocket (handshake, connected, closed)
  volatile enum ws_state state;
  //! information about the last received message
  struct last_message lastMessage;
  //! pointer to the connection user data
  void *connectionUserData;
  //! stores the time for message timeouts
  struct timespec timeout;
  union
  {
    //!pointer to the websocket client descriptor (in case of client mode)
    struct websocket_client_desc *wsClientDesc;
    //!pointer to the websocket server descriptor (in case of server mode)
    struct websocket_server_desc *wsServerDesc;
  } wsDesc;
};

struct websocket_client_desc
{
  //! callback that is called when a message is received on the websocket
  void (*ws_onMessage)(void *socketUserData, struct websocket_connection_desc *connectionDesc, void *connectionUserData,
                       enum ws_data_type dataType, void *msg, size_t len);
  //! callback that is called when the websocket is connected
  void* (*ws_onOpen)(void *socketUserData, struct websocket_connection_desc *connectionDesc);
  //! callback that is called when the websocket is closed
  void (*ws_onClose)(void *socketUserData, struct websocket_connection_desc *connectionDesc, void *connectionUserData);
  //! pointer to the socket descriptor
  void *socketDesc;
  //! pointer to the user data
  void *wsUserData;
  //! the websocket connection descriptor for the current connection
  struct websocket_connection_desc *connection;
  //! the address of the host
  char *address;
  //! the port of the websocket host
  char *port;
  //! the endpoint where the websocket should connect
  char *endpoint;
  //! pointer to the websocket key that is used to validate it's a websocket connection
  char *wsKey;
};

#define WS_ACCEPT_MAGIC_KEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/**
 * \brief Calculates the Sec-WebSocket-Accept from the Sec-WebSocket-Key
 *
 * \param *key Pointer to the key that should be used
 *
 * \return The string containing the Sec-WebSocket-Accept (must be freed after use)
 */
static char* calculateSecWebSocketAccept(const char *key)
{
  char concatString[64];
  unsigned char sha1Hash[21];

  snprintf(concatString, sizeof(concatString), "%s" WS_ACCEPT_MAGIC_KEY, key);

  SHA1((char*)sha1Hash, concatString, strlen(concatString));

  return base64_encode(sha1Hash, 20);
}

//GET /chat HTTP/1.1
//Host: example.com:8000
//Upgrade: websocket
//Connection: Upgrade
//Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
//Sec-WebSocket-Version: 13

#define WS_HS_KEY_ID "Sec-WebSocket-Key:"
#define WS_HS_KEY_LEN 25

/*
 * \brief Parses the http header and extracts the Sec-WebSocket-Key
 *
 * \param *wsHeader Pointer to the string with the header
 * \param len Length of the header
 * \param[out] *key Pointer to where the key should be stored (should be at least WS_HS_KEY_LEN big)
 *
 * \return 0 if successful else -1
 *
 */
static int parseHttpHeader(const char *wsHeader, size_t len, char *key)
{
  const char *cpnt;
  int i;

  cpnt = strnstr((char*)wsHeader, WS_HS_KEY_ID, len);
  if(!cpnt)
  {
    log_err("%s() couldn't find key", __func__);
    return -1;
  }

  cpnt += strlen(WS_HS_KEY_ID);

  while(!isgraph(*cpnt))
  {
    cpnt++;
  }

  for(i = 0; (i < WS_HS_KEY_LEN - 1) && isgraph(cpnt[i]) && ((size_t)(&cpnt[i] - wsHeader) < len); i++)
  {
    key[i] = cpnt[i];
  }

  if(i < WS_HS_KEY_LEN - 1)
    return -1;

  key[i] = '\0';

#ifdef VERBOSE_MODE
  log_dbg("%s() key:%s", __func__, key);
#endif

  return 0;
}

#define WS_HANDSHAKE_REPLY_BLUEPRINT "HTTP/1.1 101 Switching Protocols\r\n" \
                                     "Upgrade: websocket\r\n" \
                                     "Connection: Upgrade\r\n" \
                                     "Sec-WebSocket-Accept: %s\r\n" \
                                     "\r\n"

/**
 * \brief Sends the websocket handshake reply
 *
 * \param *socketConnectionDesc The connection descriptor of the socket
 * \param *replyKey The calculated Sec-WebSocket-Accept key
 *
 * \return -1 on error 0 if successful
 *
 */
static int sendWsHandshakeReply(struct socket_connection_desc *socketConnectionDesc, const char *replyKey)
{
  char replyHeader[strlen(WS_HANDSHAKE_REPLY_BLUEPRINT) + 28];

  if(snprintf(replyHeader, sizeof(replyHeader), WS_HANDSHAKE_REPLY_BLUEPRINT, replyKey) >= (int)sizeof(replyHeader))
  {
    log_err("problem with the handshake reply key (buffer to small)");
    return -1;
  }

  return socketServer_send(socketConnectionDesc, replyHeader, strlen(replyHeader));
}

#define WS_HS_REPLY_ID "Sec-WebSocket-Accept:"


/**
 * \brief checks if we've received the correct handshake reply
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param *header Pointer to the header
 * \param[in-out] len Input the length of the message output the length of the header
 *
 * \return True => handshake correct else false
 */
static bool checkWsHandshakeReply(struct websocket_connection_desc *wsConnectionDesc, char *header, size_t *len)
{
  if(wsConnectionDesc->wsType == WS_TYPE_SERVER)
    return false;

  struct websocket_client_desc *wsDesc = wsConnectionDesc->wsDesc.wsClientDesc;

  char *cpnt;
  unsigned long i;
  char key[30];
  char *acceptString = NULL;
  bool retVal = false;

  cpnt = strnstr(header, WS_HS_REPLY_ID, *len);
  if(!cpnt)
  {
    log_err("%s() couldn't find key", __func__);
    return false;
  }

  cpnt += strlen(WS_HS_REPLY_ID);

  while(!isgraph(*cpnt))
  {
    cpnt++;

    if((size_t)(cpnt - header) >= *len)
      return false;
  }

  for(i = 0; (i < sizeof(key) - 1) && isgraph(cpnt[i]) && ((size_t)(&cpnt[i] - header) < *len); i++)
  {
    key[i] = cpnt[i];
  }

  key[i] = '\0';

  cpnt = strnstr(header, "\r\n\r\n", *len);
  if(cpnt == NULL)
    return false;
  cpnt += strlen("\r\n\r\n");

  *len = (uintptr_t)cpnt - (uintptr_t)header;

  acceptString = calculateSecWebSocketAccept(wsDesc->wsKey);
  if(acceptString == NULL)
  {
    log_err("calculateSecWebSocketAccept failed");
    return false;
  }

  retVal = (strcmp(key, acceptString) == 0);

  free(acceptString);
  return retVal;
}


/**
 * \brief Sends the websocket handshake request
 *
 * \param *wsDesc Pointer to the websocket client descriptor
 *
 * \return True if successful else false
 */
static bool sendWsHandshakeRequest(struct websocket_connection_desc *wsConnectionDesc)
{
  unsigned char wsKeyBytes[16];
  unsigned long i;
  char *requestHeader = NULL;
  bool success = false;
  struct websocket_client_desc *wsDesc = wsConnectionDesc->wsDesc.wsClientDesc;

  for(i = 0; i < sizeof(wsKeyBytes); i++)
  {
    wsKeyBytes[i] = rand();
  }

  wsDesc->wsKey = base64_encode(wsKeyBytes, sizeof(wsKeyBytes));
  if(asprintf(&requestHeader, "GET %s HTTP/1.1\r\n"
      "Host: %s:%s\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: %s\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n", wsDesc->endpoint, wsDesc->address, wsDesc->port, wsDesc->wsKey) < 0)
  {
    log_err("asprintf failed");
    goto EXIT;
  }

  if(socketClient_send(wsConnectionDesc->socketClientDesc, requestHeader, strlen(requestHeader)) == -1)
  {
    log_err("socketClient_send failed");
    goto EXIT;
  }

  success = true;

  EXIT: if(!success)
  {
    free(wsDesc->wsKey);
    wsDesc->wsKey = NULL;
  }
  free(requestHeader);
  return success;
}


// Frame format of a websocket:
//​​
//      0                   1                   2                   3
//      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//     +-+-+-+-+-------+-+-------------+-------------------------------+
//     |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
//     |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
//     |N|V|V|V|       |S|             |   (if payload len==126/127)   |
//     | |1|2|3|       |K|             |                               |
//     +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
//     |     Extended payload length continued, if payload len == 127  |
//     + - - - - - - - - - - - - - - - +-------------------------------+
//     |                               |Masking-key, if MASK set to 1  |
//     +-------------------------------+-------------------------------+
//     | Masking-key (continued)       |          Payload Data         |
//     +-------------------------------- - - - - - - - - - - - - - - - +
//     :                     Payload Data continued ...                :
//     + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
//     |                     Payload Data continued ...                |
//     +---------------------------------------------------------------+
//
// copied form
// https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API/Writing_WebSocket_servers
// licensed under CC-BY-SA 2.5.

struct ws_header
{
  //! fin flag received
  bool fin;
  //! the websocket opcode
  enum ws_opcode opcode;
  //! the length of the payload
  size_t payloadLength;
  //! indicates if the message is masked
  bool masked;
  //! the mask (undefined if not masked)
  unsigned char mask[4];
  //! the offset of the payload
  unsigned char payloadStartOffset;
};

/**
 * \brief Prints the websocket header (for debugging purpose only)
 *
 * \param *header Pointer to the header
 */
static void  __attribute__((unused)) printWsHeader(const struct ws_header *header)
{
  log_dbg("----ws header----");
  log_dbg("opcode:%d", header->opcode);
  log_dbg("fin:%d", header->fin);
  log_dbg("masked:%d", header->masked);
  log_dbg("pllength:%zu", header->payloadLength);
  log_dbg("ploffset:%u", header->payloadStartOffset);
  log_dbg("-----------------");
}

/**
 * \brief Parses the header of a websocket message
 *
 * \param *data Pointer to the message received from the socket
 * \param len The length of the data
 * \param[out] *header Pointer to where the header should be written to
 *
 * \return 1 if successful 0 if msg to short else -1
 */
static int parseWebsocketHeader(const unsigned char *data, size_t len, struct ws_header *header)
{
  header->fin = (data[0] & 0x80) ? true : false;
  header->opcode = data[0] & 0x0F;
  if(data[0] & 0x70) //reserved bits must be 0
  {
    log_err("reserved bits must be 0");
    return -1;
  }
  switch(header->opcode)
  {
    case WS_OPCODE_CONTINUATION:
    case WS_OPCODE_TEXT:
    case WS_OPCODE_BINARY:
    case WS_OPCODE_PING:
    case WS_OPCODE_PONG:
    case WS_OPCODE_DISCONNECT:
      break;

    default:
      log_err("opcode unknown (%d)", header->opcode);
      return -1;
  }

  header->masked = (data[1] & 0x80) ? true : false;
  size_t i, lengthNumBytes;

  if(len < 2)
  {
    return 0;
  }

  header->payloadLength = 0;

  //decode payload length
  if((data[1] & 0x7F) <= MAX_DEFAULT_PAYLOAD_LENGTH)
  {
    header->payloadLength = data[1] & 0x7F;
    lengthNumBytes = 0; //not really true but needed for further calculations
  }
  else if((data[1] & 0x7F) == EXTENDED_16BIT_PAYLOAD_LENGTH)
  {
    if(len < 4)
    {
      return 0;
    }
    lengthNumBytes = 2;
  }
  else //data[1] == EXTENDED_64BIT_PAYLOAD_LENGTH
  {
    if(len < 10)
    {
      return 0;
    }
    lengthNumBytes = 8;
  }

  for(i = 0; i < lengthNumBytes; i++)
  {
    header->payloadLength <<= 8;
    header->payloadLength |= data[2 + i];
  }

#ifdef VERBOSE_MODE
  log_dbg("payloadlength:%zu", header->payloadLength);
#endif

  if(header->masked)
  {
    if(len < 2 + lengthNumBytes + 4)
    {
      return 0;
    }
    for(i = 0; i < 4; i++)
    {
      header->mask[i] = data[2 + lengthNumBytes + i];
    }

    header->payloadStartOffset = 2 + lengthNumBytes + 4;
  }
  else
    header->payloadStartOffset = 2 + lengthNumBytes;

  return 1;
}

/**
 * \brief creates the websocket header from the given variables
 *
 * \param[out] *buffer Pointer to the buffer where the header should be written to
 *                 should be able to hold at least 10 bytes
 * \param opcode The opcode that should be used for the header
 * \param fin Fin bit (is this the last frame (true) or will more follow (false))
 * \param masked True => add a mask, false => add no mask
 * \param mask The mask that should be used (ignored in case masked is false)
 * \param len The length of the payload
 *
 * \return The length of the header
 */
static int createWebsocketHeader(unsigned char *buffer, enum ws_opcode opcode, bool fin, bool masked,
                                 unsigned long mask, size_t len)
{
  int cnt, i;

  buffer[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);

  //masked bit always 0 for server->client replies
  if(len <= MAX_DEFAULT_PAYLOAD_LENGTH)
  {
    buffer[1] = len;
    cnt = 2;
  }
  else if(len <= 0xFFFF)
  {
    buffer[1] = EXTENDED_16BIT_PAYLOAD_LENGTH;
    buffer[2] = len >> 8;
    buffer[3] = len & 0xFF;
    cnt = 4;
  }
  else
  {
    buffer[1] = EXTENDED_64BIT_PAYLOAD_LENGTH;
    cnt = 2;

    for(i = 7; i > -1; i--)
    {
      buffer[cnt] = (len >> (i * 8)) & 0xFF;
      cnt++;
    }
  }

  if(masked)
  {
    buffer[1] |= 0x80;
    for(i = 3; i > -1; i--)
    {
      buffer[cnt] = (mask >> (i * 8)) & 0xFF;
      cnt++;
    }
  }

  return cnt;
}

/**
 * \brief Copies the data in the buffer pointed by from to the buffer pointed by to and mask
 *        them
 *
 * \param *to Pointer to where the data should copied masked
 * \param *from Pointer to the original data
 * \param mask The mask that shoud be used (32-bit)
 * \param len The length of the data that should be copied
 *
 * \note The masking algorithm is big endian XOR
 */
static void copyMasked(unsigned char *to, const unsigned char *from, unsigned long mask, size_t len)
{
  unsigned char byteMask[4];
  size_t i;
  unsigned char maskIdx = 0;

  //big endian
  byteMask[0] = mask >> 24 & 0xFF;
  byteMask[1] = mask >> 16 & 0xFF;
  byteMask[2] = mask >> 8 & 0xFF;
  byteMask[3] = mask >> 0 & 0xFF;

  for(i = 0; i < len; i++)
  {
    *to = *from ^ byteMask[maskIdx];
    maskIdx = (maskIdx + 1) % 4;
    to++;
    from++;
  }
}

/**
 * \brief Sends data through websockets with custom opcodes
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param opcode The opcode to use
 * \param fin True if this is the last frame of a sequence else false
 * \param masked True => send masked (client to server) else false (server to client)
 * \param *msg The payload data
 * \param len The payload length
 *
 * \return 0 if successful else -1
 */
static int sendDataLowLevel(struct websocket_connection_desc *wsConnectionDesc, enum ws_opcode opcode, bool fin, bool masked, const void *msg,
                            size_t len)
{
  unsigned char header[14];  //the maximum size of a websocket header is 14
  int headerLength;
  unsigned char *sendBuffer;
  int rc = -1;
  unsigned long mask = 0;

  if(wsConnectionDesc->state == WS_STATE_CLOSED)
    return -1;

  if(masked)
  {
    mask = (rand() << 16) | (rand() & 0x0000FFFF);
  }

  headerLength = createWebsocketHeader(header, opcode, fin, masked, mask, len);

  sendBuffer = malloc(headerLength + len);
  if(!sendBuffer)
    return -1;
  memcpy(sendBuffer, header, headerLength);
  if(len)
  {
    if(masked)
    {
      copyMasked(&sendBuffer[headerLength], msg, mask, len);
    }
    else
      memcpy(&sendBuffer[headerLength], msg, len);
  }

  switch(wsConnectionDesc->wsType)
  {
    case WS_TYPE_SERVER:
      rc = socketServer_send(wsConnectionDesc->socketClientDesc, sendBuffer, len + headerLength);
      break;

    case WS_TYPE_CLIENT:
      rc = socketClient_send(wsConnectionDesc->socketClientDesc, sendBuffer, len + headerLength);
      break;
  }
  free(sendBuffer);

#ifdef VERBOSE_MODE
  log_dbg("%s retv:%d", __func__, rc);
#endif

  return rc;
}

/**
 * \brief Checks if the given close code is valid
 *
 * \param code The code that should be checked
 *
 * \return True if code is valid else false
 */
static bool checkCloseCode(enum ws_close_code code)
{
  if(code < 1000)
    return false;

  if(code > 4999)
    return false;

  if((code >= 1012) & (code <= 1014))
    return false;

  if((code >= 1016) && (code < 3000))
    return false;

  switch(code)
  {
    case WS_CLOSE_CODE_RESERVED_0:
    case WS_CLOSE_CODE_RESERVED_1:
    case WS_CLOSE_CODE_RESERVED_2:
    case WS_CLOSE_CODE_RESERVED_3:
      return false;

    default:
      return true;
  }
}

enum ws_msg_state
{
  WS_MSG_STATE_ERROR,
  WS_MSG_STATE_INCOMPLETE,
  WS_MSG_STATE_NO_USER_DATA,
  WS_MSG_STATE_USER_DATA,
};

/**
 * \brief Handles the first message (which is sometimes followed by a cont message)
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param *data Pointer to the payload data
 * \param *header Pointer to the parsed websocket header structure
 *
 * \return The message state
 */
static enum ws_msg_state handleFirstMessage(struct websocket_connection_desc *wsConnectionDesc, const unsigned char *data,
                                            struct ws_header *header)
{
  size_t i;

  if(!header->masked && (wsConnectionDesc->wsType == WS_TYPE_SERVER))
  {
    websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(wsConnectionDesc->lastMessage.data != NULL)
  {
    log_err("last message not finished");
    websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(header->payloadLength) //it's allowed to send frames with payload length = 0
  {
    if(header->fin)
      wsConnectionDesc->lastMessage.data = refcnt_allocate(header->payloadLength, NULL);
    else
      wsConnectionDesc->lastMessage.data = malloc(header->payloadLength);
    if(!wsConnectionDesc->lastMessage.data)
    {
      log_err("refcnt_allocate failed dropping message");
      return WS_MSG_STATE_ERROR;
    }

    if(header->masked)
    {
      for(i = 0; i < header->payloadLength; i++)
      {
        wsConnectionDesc->lastMessage.data[i] = data[header->payloadStartOffset + i] ^ header->mask[i % 4];
      }
    }
    else
    {
      memcpy(wsConnectionDesc->lastMessage.data, &data[header->payloadStartOffset], header->payloadLength);
    }
  }

  wsConnectionDesc->lastMessage.firstReceived = true;
  wsConnectionDesc->lastMessage.complete = header->fin;
  wsConnectionDesc->lastMessage.dataType = header->opcode == WS_OPCODE_TEXT ? WS_DATA_TYPE_TEXT : WS_DATA_TYPE_BINARY;

  wsConnectionDesc->lastMessage.len = header->payloadLength;
  if(wsConnectionDesc->lastMessage.dataType == WS_DATA_TYPE_TEXT)
  {
    enum utf8_state state;

    wsConnectionDesc->lastMessage.utf8Handle = 0;
    state = utf8_validate(wsConnectionDesc->lastMessage.data, wsConnectionDesc->lastMessage.len,
        &wsConnectionDesc->lastMessage.utf8Handle);
    if((header->fin && (state != UTF8_STATE_OK)) || (!header->fin && (state == UTF8_STATE_FAIL)))
    {
      log_err("no valid utf8 string closing connection");
      websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_INVALID_DATA);
      return WS_MSG_STATE_ERROR;
    }
  }
  if(header->fin)
    return WS_MSG_STATE_USER_DATA;
  else
    return WS_MSG_STATE_NO_USER_DATA;
}

/**
 * \brief Handles a cont message
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param *data Pointer to the payload data
 * \param *header Pointer to the parsed websocket header structure
 *
 * \return The message state
 */
static enum ws_msg_state handleContMessage(struct websocket_connection_desc *wsConnectionDesc, const unsigned char *data,
                                           struct ws_header *header)
{
  size_t i;
  char *temp;

  if(!wsConnectionDesc->lastMessage.firstReceived)
  {
    log_err("missing last message closing connection");
    websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if((wsConnectionDesc->wsType != WS_TYPE_SERVER) == header->masked)
  {
    log_err("mask bit wrong");
    websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(wsConnectionDesc->lastMessage.len + header->payloadLength) //it's allowed to send frames with payload length = 0
  {
    if(header->fin)
    {
      temp = refcnt_allocate(wsConnectionDesc->lastMessage.len + header->payloadLength, NULL);
      if(!temp)
      {
        log_err("refcnt_allocate failed dropping message");
        free(wsConnectionDesc->lastMessage.data);
        wsConnectionDesc->lastMessage.data = NULL;
        return WS_MSG_STATE_ERROR;
      }
      memcpy(temp, wsConnectionDesc->lastMessage.data, wsConnectionDesc->lastMessage.len);
      free(wsConnectionDesc->lastMessage.data);
      wsConnectionDesc->lastMessage.data = temp;
    }
    else
    {
      temp = realloc(wsConnectionDesc->lastMessage.data, wsConnectionDesc->lastMessage.len + header->payloadLength);
      if(!temp)
      {
        log_err("realloc failed dropping message");
        free(wsConnectionDesc->lastMessage.data);
        wsConnectionDesc->lastMessage.data = NULL;
        return WS_MSG_STATE_ERROR;
      }
      wsConnectionDesc->lastMessage.data = temp;
    }

    if(header->masked)
    {
      for(i = 0; i < header->payloadLength; i++)
      {
        wsConnectionDesc->lastMessage.data[wsConnectionDesc->lastMessage.len + i] = data[header->payloadStartOffset + i]
            ^ header->mask[i % 4];
      }
    }
    else
    {
      memcpy(&wsConnectionDesc->lastMessage.data[wsConnectionDesc->lastMessage.len], &data[header->payloadStartOffset],
          header->payloadLength);
    }
  }
  wsConnectionDesc->lastMessage.complete = header->fin;

  if(wsConnectionDesc->lastMessage.dataType == WS_DATA_TYPE_TEXT)
  {
    enum utf8_state state;
    state = utf8_validate(&wsConnectionDesc->lastMessage.data[wsConnectionDesc->lastMessage.len], header->payloadLength,
        &wsConnectionDesc->lastMessage.utf8Handle);
    if((header->fin && state != UTF8_STATE_OK) || (!header->fin && state == UTF8_STATE_FAIL))
    {
      log_err("no valid utf8 string closing connection");
      websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_INVALID_DATA);
      return WS_MSG_STATE_ERROR;
    }
  }
  wsConnectionDesc->lastMessage.len += header->payloadLength;

  if(header->fin)
    return WS_MSG_STATE_USER_DATA;
  else
    return WS_MSG_STATE_NO_USER_DATA;
}

/**
 * \brief Handles a ping message and replies with a pong message
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param *data Pointer to the payload data
 * \param *header Pointer to the parsed websocket header structure
 *
 * \return The message state
 */
static enum ws_msg_state handlePingMessage(struct websocket_connection_desc *wsConnectionDesc, const unsigned char *data,
                                           struct ws_header *header)
{
  int rc;
  char *temp;
  size_t i;
  bool masked = (wsConnectionDesc->wsType == WS_TYPE_CLIENT);

  if(header->fin)
  {
    if(header->payloadLength > MAX_DEFAULT_PAYLOAD_LENGTH)
    {
      websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
      return WS_MSG_STATE_NO_USER_DATA;
    }
    else if(header->masked)
    {
      if(header->payloadLength)
      {
        temp = malloc(header->payloadLength);
        if(!temp)
        {
          log_err("malloc failed dropping message");
          return WS_MSG_STATE_ERROR;
        }

        for(i = 0; i < header->payloadLength; i++)
        {
          temp[i] = data[header->payloadStartOffset + i] ^ header->mask[i % 4];
        }
      }
      else
        temp = NULL;

      if(sendDataLowLevel(wsConnectionDesc, WS_OPCODE_PONG, true, masked, temp, header->payloadLength) == 0)
        rc = WS_MSG_STATE_NO_USER_DATA;
      else
        rc = WS_MSG_STATE_ERROR;
      free(temp);
      return rc;
    }
    else
    {
      if(sendDataLowLevel(wsConnectionDesc, WS_OPCODE_PONG, true, masked, &data[header->payloadStartOffset],
          header->payloadLength) == 0)
        return WS_MSG_STATE_NO_USER_DATA;
      else
        return WS_MSG_STATE_ERROR;
    }
  }
  else
  {
    websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }
}

/**
 * \brief Handles a pong message
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param *data Pointer to the payload data
 * \param *header Pointer to the parsed websocket header structure
 *
 * \return the message state
 */
static enum ws_msg_state handlePongMessage(struct websocket_connection_desc *wsConnectionDesc, const unsigned char *data,
                                           struct ws_header *header)
{
  (void) data;

  if(header->fin && (header->payloadLength <= MAX_DEFAULT_PAYLOAD_LENGTH))
  {
    //Pongs are ignored now because actually we also don't send pings
    return WS_MSG_STATE_NO_USER_DATA;
  }
  else
  {
    websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }
}

/**
 * \brief handles a disconnect message and replies with a disconnect message
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param *data Pointer to the payload data
 * \param *header Pointer to the parsed websocket header structure
 *
 * \return the message state
 */
static enum ws_msg_state handleDisconnectMessage(struct websocket_connection_desc *wsConnectionDesc,
                                                 const unsigned char *data, struct ws_header *header)
{
  size_t i;
  int rc;
  unsigned long utf8Handle = 0;
  bool masked = (wsConnectionDesc->wsType == WS_TYPE_CLIENT);

  if((header->fin) && (header->payloadLength != 1) && (header->payloadLength <= MAX_DEFAULT_PAYLOAD_LENGTH))
  {
    enum ws_close_code code;

    if(!header->payloadLength)
    {
      websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_NORMAL);
      return WS_MSG_STATE_NO_USER_DATA;
    }
    else if(header->masked == (wsConnectionDesc->wsType == WS_TYPE_SERVER))
    {
      char tempBuffer[MAX_DEFAULT_PAYLOAD_LENGTH];

      if(header->masked)
      {
        for(i = 0; i < header->payloadLength; i++)
        {
          tempBuffer[i] = data[header->payloadStartOffset + i] ^ header->mask[i % 4];
        }
      }
      else
      {
        memcpy(tempBuffer, &data[header->payloadStartOffset], header->payloadLength);
      }

      code = (((unsigned char)tempBuffer[0]) << 8) | ((unsigned char)tempBuffer[1]);
      if(checkCloseCode(code) == false)
      {
        websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
        return WS_MSG_STATE_ERROR;
      }
      else if((header->payloadLength == 2)
          || (UTF8_STATE_OK == utf8_validate(&tempBuffer[2], header->payloadLength - 2, &utf8Handle)))
      {
        websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_NORMAL);
        return WS_MSG_STATE_NO_USER_DATA;
      }
      else
      {
        websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_INVALID_DATA);
        return WS_MSG_STATE_ERROR;
      }
    }
    else
    {
      if((header->payloadLength == 2)
          || utf8_validate((char*)&data[header->payloadStartOffset + 2], header->payloadLength - 2, &utf8Handle))
      {
        if(sendDataLowLevel(wsConnectionDesc, WS_OPCODE_DISCONNECT, true, masked, &data[header->payloadStartOffset],
            header->payloadLength) == 0)
          rc = WS_MSG_STATE_NO_USER_DATA;
        else
          rc = WS_MSG_STATE_ERROR;
        switch(wsConnectionDesc->wsType)
        {
          case WS_TYPE_SERVER:
            socketServer_closeConnection(wsConnectionDesc->socketClientDesc);
            break;

          case WS_TYPE_CLIENT:
            socketClient_closeConnection(wsConnectionDesc->wsDesc.wsClientDesc->socketDesc);
            break;
        }
      }
      else
      {
        websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_INVALID_DATA);
        rc = WS_MSG_STATE_ERROR;
      }
      return rc;
    }
  }
  else
  {
    websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }
}

/**
 * \brief parses a message and stores it to the client descriptor
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param *data Pointer to the received data
 * \param len The length of the data
 * \param header The parsed header struct (as parsed by parseWebsocketHeader)
 *
 * \return One of WS_MSG_STATE_x
 */
static enum ws_msg_state parseMessage(struct websocket_connection_desc *wsConnectionDesc, const unsigned char *data,
                                      size_t len, struct ws_header *header)
{
  enum ws_msg_state rc;

#ifdef VERBOSE_MODE
  log_dbg("->len:%zu", len);
#endif

  if(len < header->payloadStartOffset + header->payloadLength)
    return WS_MSG_STATE_INCOMPLETE;

  switch(header->opcode)
  {
    case WS_OPCODE_TEXT:
    case WS_OPCODE_BINARY:
      return handleFirstMessage(wsConnectionDesc, data, header);

    case WS_OPCODE_CONTINUATION:
      return handleContMessage(wsConnectionDesc, data, header);

    case WS_OPCODE_PING:
      return handlePingMessage(wsConnectionDesc, data, header);

    case WS_OPCODE_PONG:
      return handlePongMessage(wsConnectionDesc, data, header);

    case WS_OPCODE_DISCONNECT:
      return handleDisconnectMessage(wsConnectionDesc, data, header);

    default:
      log_err("unknown opcode (%d)", header->opcode);
      rc = WS_MSG_STATE_ERROR;
      break;
  }

  return rc;
}

/**
 * \brief Function that gets called when a connection to a client is established
 *         allocates and initialises the wsClientDesc
 *
 * \param *socketUserData: In this case this is the websocket descriptor
 * \param *socketConnectionDesc The connection descriptor from the socket server
 *
 * \return Pointer to the websocket connection descriptor
 */
static void* websocketServer_onOpen(void *socketUserData, struct socket_connection_desc *socketConnectionDesc)
{
  struct websocket_server_desc *wsDesc = socketUserData;
  struct websocket_connection_desc *wsConnectionDesc;

  if(wsDesc == NULL)
  {
    log_err("%s(): wsDesc must not be NULL!", __func__);
    return NULL;
  }

  if(socketConnectionDesc == NULL)
  {
    log_err("%s(): socketClientDesc must not be NULL!", __func__);
    return NULL;
  }

  refcnt_ref(socketConnectionDesc);
  wsConnectionDesc = refcnt_allocate(sizeof(struct websocket_connection_desc), NULL);
  memset(wsConnectionDesc, 0, sizeof(struct websocket_connection_desc));
  wsConnectionDesc->wsType = WS_TYPE_SERVER;
  wsConnectionDesc->socketClientDesc = socketConnectionDesc;
  wsConnectionDesc->state = WS_STATE_HANDSHAKE;
  wsConnectionDesc->timeout.tv_nsec = 0;
  wsConnectionDesc->timeout.tv_sec = 0;
  wsConnectionDesc->lastMessage.firstReceived = false;
  wsConnectionDesc->lastMessage.data = NULL;
  wsConnectionDesc->lastMessage.len = 0;
  wsConnectionDesc->lastMessage.complete = false;
  wsConnectionDesc->wsDesc.wsServerDesc = wsDesc;

  return wsConnectionDesc;
}

/**
 * \brief Function that get's called when a websocket client connection is
 *        established
 *
 * \param *socketUserData In this case this is the websocket descriptor
 * \param *socketDesc The socket descriptor of the socket client
 *
 * \return Pointer to the websocket connection descriptor
 */
static void* websocketClient_onOpen(void *socketUserData, void *socketDesc)
{
  struct websocket_connection_desc *wsConnectionDesc = socketUserData;
  struct websocket_client_desc *wsDesc = wsConnectionDesc->wsDesc.wsClientDesc;
  (void)socketDesc;

  if(sendWsHandshakeRequest(wsConnectionDesc))
  {
    return wsDesc->connection;
  }
  else
  {
    wsConnectionDesc->state = WS_STATE_CLOSED;
    return NULL;
  }
}

/**
 * \brief gets called when the websocket is closed
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 */
static void callOnClose(struct websocket_connection_desc *wsConnectionDesc)
{
  switch(wsConnectionDesc->wsType)
  {
    case WS_TYPE_SERVER:
      if(wsConnectionDesc->wsDesc.wsServerDesc->ws_onClose != NULL)
      {
        wsConnectionDesc->wsDesc.wsServerDesc->ws_onClose(wsConnectionDesc->wsDesc.wsServerDesc,
            wsConnectionDesc->wsDesc.wsServerDesc->wsSocketUserData, wsConnectionDesc,
            wsConnectionDesc->connectionUserData);
      }
      else if(wsConnectionDesc->wsDesc.wsServerDesc->ws_onCloseLegacy != NULL)
      {
        wsConnectionDesc->wsDesc.wsServerDesc->ws_onCloseLegacy(wsConnectionDesc->wsDesc.wsServerDesc->wsSocketUserData,
            wsConnectionDesc, wsConnectionDesc->connectionUserData);
      }
      break;

    case WS_TYPE_CLIENT:
      if(wsConnectionDesc->wsDesc.wsClientDesc->ws_onClose != NULL)
        wsConnectionDesc->wsDesc.wsClientDesc->ws_onClose(wsConnectionDesc->wsDesc.wsClientDesc->wsUserData, wsConnectionDesc,
            wsConnectionDesc->connectionUserData);
      break;
  }
}

/**
 * \brief function that gets called when a connection to a client is closed
 *         frees the websocket client descriptor
 *
 * \param *socketUserData The websocket descriptor
 * \param *socketClientDesc The connection descriptor from the socket server/client
 * \param *wsConnectionDescriptor The websocket connection descriptor
 *
 */
static void websocket_onClose(void *socketUserData, void *socketConnectionDesc, void *wsConnectionDescriptor)
{
  (void) socketUserData;
  struct websocket_connection_desc *wsConnectionDesc = wsConnectionDescriptor;
  (void)socketConnectionDesc;

  if(wsConnectionDesc == NULL)
  {
    log_err("%s(): wsConnectionDesc must not be NULL!", __func__);
    return;
  }

  if(wsConnectionDesc->lastMessage.data && wsConnectionDesc->lastMessage.complete)
    refcnt_unref(wsConnectionDesc->lastMessage.data);
  else
    free(wsConnectionDesc->lastMessage.data);
  wsConnectionDesc->lastMessage.data = NULL;

  if(wsConnectionDesc->state == WS_STATE_CONNECTED)
  {
    wsConnectionDesc->state = WS_STATE_CLOSED;

    callOnClose(wsConnectionDesc);
  }

  if(wsConnectionDesc->wsType == WS_TYPE_SERVER) //in client mode the connection descriptor is not allocated
  {
    if(wsConnectionDesc->socketClientDesc)
      refcnt_unref(wsConnectionDesc->socketClientDesc);
    wsConnectionDesc->socketClientDesc = NULL;
    refcnt_unref(wsConnectionDesc);
  }
}

/**
 * \brief Gets called when a message was received on the websocket
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 */
static void callOnMessage(struct websocket_connection_desc *wsConnectionDesc)
{
  switch(wsConnectionDesc->wsType)
  {
    case WS_TYPE_SERVER:
    {
      if(wsConnectionDesc->wsDesc.wsServerDesc->ws_onMessage)
      {
        wsConnectionDesc->wsDesc.wsServerDesc->ws_onMessage(wsConnectionDesc->wsDesc.wsServerDesc->wsSocketUserData,
            wsConnectionDesc, wsConnectionDesc->connectionUserData, wsConnectionDesc->lastMessage.dataType,
            wsConnectionDesc->lastMessage.data, wsConnectionDesc->lastMessage.len);
      }
    }
      break;

    case WS_TYPE_CLIENT:
    {
      if(wsConnectionDesc->wsDesc.wsClientDesc->ws_onMessage)
      {
        wsConnectionDesc->wsDesc.wsClientDesc->ws_onMessage(wsConnectionDesc->wsDesc.wsServerDesc->wsSocketUserData,
            wsConnectionDesc, wsConnectionDesc->connectionUserData, wsConnectionDesc->lastMessage.dataType,
            wsConnectionDesc->lastMessage.data, wsConnectionDesc->lastMessage.len);
      }
    }
      break;
  }
}

/**
 * \brief Function that gets called when a message arrives at the socket server
 *
 * \param *socketUserData In this case this is the websocket descriptor
 * \param *socketConnectionDesc The descriptor of the underlying socket
 * \param *connectionDescriptor The websocket connection descriptor
 * \param *msg Pointer to the buffer containing the data
 * \param len The length of msg
 *
 * \return the amount of bytes read
 *
 */
static size_t websocket_onMessage(void *socketUserData, void *socketConnectionDesc, void *connectionDescriptor, void *msg, size_t len)
{
  struct websocket_connection_desc *wsConnectionDesc = connectionDescriptor;
  struct ws_header wsHeader = {0};
  struct timespec now;

  char key[WS_HS_KEY_LEN];
  char *replyKey;

  if(wsConnectionDesc == NULL)
  {
    log_err("%s(): wsConnectionDesc must not be NULL!", __func__);
    return 0;
  }

  if(socketConnectionDesc == NULL)
  {
    log_err("%s(): socketConnectionDesc must not be NULL!", __func__);
    return 0;
  }

  switch(wsConnectionDesc->state)
  {
    case WS_STATE_HANDSHAKE:
      switch(wsConnectionDesc->wsType)
      {
        case WS_TYPE_SERVER:
          if(parseHttpHeader(msg, len, key) == 0)
          {
            struct websocket_server_desc *wsDesc = socketUserData;

            replyKey = calculateSecWebSocketAccept(key);
            if(replyKey == NULL)
            {
              log_err("%s(): calculateSecWebSocketAccept failed!", __func__);
              return 0;
            }

#ifdef VERBOSE_MODE
                  log_dbg("%s() replyKey:%s", __func__, replyKey);
#endif

            sendWsHandshakeReply(socketConnectionDesc, replyKey);

            free(replyKey);
            wsConnectionDesc->state = WS_STATE_CONNECTED;
            if(wsDesc->ws_onOpen != NULL)
              wsConnectionDesc->connectionUserData = wsDesc->ws_onOpen(wsDesc->wsSocketUserData, wsDesc, wsConnectionDesc);

            if(wsDesc->ws_onOpenLegacy != NULL)
              wsConnectionDesc->connectionUserData = wsDesc->ws_onOpenLegacy(wsDesc, wsConnectionDesc);
          }
          else
          {
            log_err("parseHttpHeader failed");
          }
          break;

        case WS_TYPE_CLIENT:
          if(checkWsHandshakeReply(wsConnectionDesc, msg, &len))
          {
            struct websocket_client_desc *wsDesc = wsConnectionDesc->wsDesc.wsClientDesc;

            wsConnectionDesc->state = WS_STATE_CONNECTED;

            if(wsDesc->ws_onOpen != NULL)
              wsConnectionDesc->connectionUserData = wsDesc->ws_onOpen(wsDesc->wsUserData, wsConnectionDesc);
            else
              wsConnectionDesc->connectionUserData = NULL;
          }
          else
          {
            log_err("checkWsHandshakeReply failed");
          }
          break;
      }
      return len;

    case WS_STATE_CONNECTED:
      switch(parseWebsocketHeader(msg, len, &wsHeader))
      {
        case -1:
          log_err("couldn't parse header");
          websocket_closeConnection(wsConnectionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
          return len;

        case 0:
          return 0;
          break;

        case 1:
          break;
      }
#ifdef VERBOSE_MODE
      printWsHeader(&wsHeader);
#endif

      switch(parseMessage(wsConnectionDesc, msg, len, &wsHeader))
      {
        case WS_MSG_STATE_NO_USER_DATA:
          wsConnectionDesc->timeout.tv_nsec = 0;
          wsConnectionDesc->timeout.tv_sec = 0;
          return wsHeader.payloadLength + wsHeader.payloadStartOffset;

        case WS_MSG_STATE_USER_DATA:
          callOnMessage(wsConnectionDesc);
          if(wsConnectionDesc->lastMessage.data)
            refcnt_unref(wsConnectionDesc->lastMessage.data);
          wsConnectionDesc->lastMessage.data = NULL;
          wsConnectionDesc->lastMessage.complete = false;
          wsConnectionDesc->lastMessage.firstReceived = false;
          wsConnectionDesc->lastMessage.len = 0;
          wsConnectionDesc->timeout.tv_nsec = 0;
          wsConnectionDesc->timeout.tv_sec = 0;
          return wsHeader.payloadLength + wsHeader.payloadStartOffset;

        case WS_MSG_STATE_INCOMPLETE:
          clock_gettime(CLOCK_MONOTONIC, &now);
          if(wsConnectionDesc->timeout.tv_sec == (wsConnectionDesc->timeout.tv_nsec == 0))
          {
            wsConnectionDesc->timeout = now;
          }
          else if(wsConnectionDesc->timeout.tv_sec > now.tv_sec + MESSAGE_TIMEOUT_S)
          {
            free(wsConnectionDesc->lastMessage.data);
            wsConnectionDesc->lastMessage.data = NULL;
            wsConnectionDesc->lastMessage.len = 0;
            wsConnectionDesc->lastMessage.complete = 0;
            wsConnectionDesc->timeout.tv_sec = 0;
            wsConnectionDesc->timeout.tv_nsec = 0;
            log_err("message timeout");
            return len;
          }
          return 0;

        case WS_MSG_STATE_ERROR:
          if(wsConnectionDesc->lastMessage.data && wsConnectionDesc->lastMessage.complete)
            refcnt_unref(wsConnectionDesc->lastMessage.data);
          else
            free(wsConnectionDesc->lastMessage.data);
          wsConnectionDesc->lastMessage.data = NULL;
          wsConnectionDesc->lastMessage.len = 0;
          wsConnectionDesc->lastMessage.complete = 0;
          wsConnectionDesc->timeout.tv_sec = 0;
          wsConnectionDesc->timeout.tv_nsec = 0;
          return len;

        default:
          log_err("unexpected return value");
          return len;
      }
      break;

    case WS_STATE_CLOSED:
      log_err("websocket closed ignoring message");
      return len;

    default:
      break;
  }

  return 0;
}


/**
 * \brief frees the given connection
 *
 * \param *connection pointer to the websocket connection descriptor
 *
 * \note this function is passed to refcnt_allocate to free the connection in
 *       case of websocket client
 */
static void freeConnection(void *connection)
{
  struct websocket_connection_desc *wsConnectionDesc = connection;

  if(wsConnectionDesc == NULL)
    return;

  if(wsConnectionDesc->wsDesc.wsClientDesc != NULL)
  {
    if(wsConnectionDesc->socketClientDesc != NULL)
    {
      socketClient_close(wsConnectionDesc->socketClientDesc);
      wsConnectionDesc->socketClientDesc = NULL;
    }

    free(wsConnectionDesc->wsDesc.wsClientDesc->address);
    wsConnectionDesc->wsDesc.wsClientDesc->address = NULL;
    free(wsConnectionDesc->wsDesc.wsClientDesc->port);
    wsConnectionDesc->wsDesc.wsClientDesc->port = NULL;
    free(wsConnectionDesc->wsDesc.wsClientDesc->endpoint);
    wsConnectionDesc->wsDesc.wsClientDesc->endpoint = NULL;
    free(wsConnectionDesc->wsDesc.wsClientDesc->wsKey);
    wsConnectionDesc->wsDesc.wsClientDesc->wsKey = NULL;
    free(wsConnectionDesc->wsDesc.wsClientDesc);
    wsConnectionDesc->wsDesc.wsClientDesc = NULL;
  }
}

/**
 * \brief Sends binary or text data through websockets
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param dataType The datatype (WS_DATA_TYPE_BINARY or WS_DATA_TYPE_TEXT)
 * \param *msg The payload data
 * \param len The payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendData(struct websocket_connection_desc *wsConnectionDesc, enum ws_data_type dataType, const void *msg, size_t len)
{
  unsigned char opcode;
  bool masked = (wsConnectionDesc->wsType == WS_TYPE_CLIENT);

  if(wsConnectionDesc->state != WS_STATE_CONNECTED)
    return -1;

  switch(dataType)
  {
    case WS_DATA_TYPE_BINARY:
      opcode = WS_OPCODE_BINARY;
      break;

    case WS_DATA_TYPE_TEXT:
      opcode = WS_OPCODE_TEXT;
      break;

    default:
      log_err("unknown data type");
      return -1;
  }

  return sendDataLowLevel(wsConnectionDesc, opcode, true, masked, msg, len);
}

/**
 * \brief Sends fragmented binary or text data through websockets
 *         use websocket_sendDataFragmetedCont for further fragments
 *
 * \param *wsConnectionDescriptor Pointer to the websocket connection descriptor
 * \param dataType The datatype (WS_DATA_TYPE_BINARY or WS_DATA_TYPE_TEXT)
 * \param *msg The payload data
 * \param len The payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendDataFragmentedStart(struct websocket_connection_desc *wsConnectionDesc, enum ws_data_type dataType, const void *msg,
                                      size_t len)
{
  unsigned char opcode;
  bool masked = (wsConnectionDesc->wsType == WS_TYPE_CLIENT);

  if(wsConnectionDesc->state != WS_STATE_CONNECTED)
    return -1;

  switch(dataType)
  {
    case WS_DATA_TYPE_BINARY:
      opcode = WS_OPCODE_BINARY;
      break;

    case WS_DATA_TYPE_TEXT:
      opcode = WS_OPCODE_TEXT;
      break;

    default:
      log_err("unknown data type");
      return -1;
  }

  return sendDataLowLevel(wsConnectionDesc, opcode, false, masked, msg, len);
}

/**
 * \brief Continues a fragmented send
 *         use sendDataFragmentedStop to stop the transmission
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param fin True => this is the last fragment else false
 * \param *msg The payload data
 * \param len The payload length
 *
 * \return 0 if successful else -1
 *
 */
int websocket_sendDataFragmentedCont(struct websocket_connection_desc *wsConnectionDesc, bool fin, const void *msg, size_t len)
{
  bool masked = (wsConnectionDesc->wsType == WS_TYPE_CLIENT);

  if(wsConnectionDesc->state != WS_STATE_CONNECTED)
    return -1;

  return sendDataLowLevel(wsConnectionDesc, WS_OPCODE_CONTINUATION, fin, masked, msg, len);
}

/**
 * \brief Closes the given websocket connection
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 * \param code The closing code
 */
void websocket_closeConnection(struct websocket_connection_desc *wsConnectionDesc, enum ws_close_code code)
{
  unsigned char help[2];
  bool masked = (wsConnectionDesc->wsType == WS_TYPE_CLIENT);

  help[0] = (unsigned long)code >> 8;
  help[1] = (unsigned long)code & 0xFF;

  sendDataLowLevel(wsConnectionDesc, WS_OPCODE_DISCONNECT, true, masked, help, 2);

  if(wsConnectionDesc->lastMessage.data && wsConnectionDesc->lastMessage.complete)
    refcnt_unref(wsConnectionDesc->lastMessage.data);
  else
    free(wsConnectionDesc->lastMessage.data);

  wsConnectionDesc->lastMessage.data = NULL;
  wsConnectionDesc->lastMessage.len = 0;
  wsConnectionDesc->lastMessage.complete = 0;

  switch(wsConnectionDesc->wsType)
  {
    case WS_TYPE_SERVER:
      socketServer_closeConnection(wsConnectionDesc->socketClientDesc);
      break;

    case WS_TYPE_CLIENT:
      socketClient_closeConnection(wsConnectionDesc->socketClientDesc);
      break;
  }
}

/**
 * \brief returns the user data of the given client
 *
 * \param *wsConnectionDesc Pointer to the websocket client descriptor
 *
 * \ŗeturn The user data of the given connection
 *
 */
void* websocket_getConnectionUserData(struct websocket_connection_desc *wsConnectionDesc)
{
  return wsConnectionDesc->connectionUserData;
}

/**
 * \brief opens a websocket server
 *
 * \param *wsInit Pointer to the init struct
 * \param *websocketUserData userData for the socket
 *
 * \return The websocket descriptor or NULL in case of error
 *         it can be passed to websocket_ref if it is used
 *         at more places
 */
struct websocket_server_desc *websocketServer_open(struct websocket_server_init *wsInit, void *websocketUserData)
{
  struct socket_server_init socketInit;
  struct websocket_server_desc *wsDesc;

  wsDesc = refcnt_allocate(sizeof(struct websocket_server_desc), NULL);
  if(!wsDesc)
  {
    log_err("refcnt_allocate failed");
    return NULL;
  }
  memset(wsDesc, 0, sizeof(struct websocket_server_desc));

  wsDesc->ws_onOpen = wsInit->ws_onOpen;
  wsDesc->ws_onClose = wsInit->ws_onClose;
  wsDesc->ws_onCloseLegacy = NULL;
  wsDesc->ws_onMessage = wsInit->ws_onMessage;
  wsDesc->wsSocketUserData = websocketUserData;

  socketInit.address = wsInit->address;
  socketInit.port = wsInit->port;
  socketInit.socket_onOpen = websocketServer_onOpen;
  socketInit.socket_onClose = websocket_onClose;
  socketInit.socket_onMessage = websocket_onMessage;

  wsDesc->socketDesc = socketServer_open(&socketInit, wsDesc);
  if(!wsDesc->socketDesc)
  {
    log_err("socketServer_open failed");
    refcnt_unref(wsDesc);
    return NULL;
  }

  return wsDesc;
}

/**
 * \brief closes the given websocket server
 *        and decreases the reference counter of wsDesc by 1
 *
 * \param *wsDesc Pointer to the websocket descriptor
 *
 */
void websocketServer_close(struct websocket_server_desc *wsDesc)
{
  socketServer_close(wsDesc->socketDesc);
  refcnt_unref(wsDesc);
}

/**
 * \brief opens a websocket client connection
 *
 * \param wsInit Pointer to the init struct
 * \param websocketUserData UserData for the socket
 *
 * \return The websocket client descriptor or NULL in case of error
 *         it can be passed to websocket_ref if it is used
 *         at more places
 */
struct websocket_connection_desc *websocketClient_open(struct websocket_client_init *wsInit, void *websocketUserData)
{
  struct socket_client_init socketInit;
  struct websocket_connection_desc *wsConnection;

  wsConnection = refcnt_allocate(sizeof(struct websocket_connection_desc), freeConnection);
  if(wsConnection == NULL)
  {
    goto ERROR;
  }
  memset(wsConnection, 0, sizeof(struct websocket_connection_desc));

  wsConnection->wsType = WS_TYPE_CLIENT;
  wsConnection->state = WS_STATE_HANDSHAKE;
  wsConnection->lastMessage.firstReceived = false;
  wsConnection->lastMessage.complete = false;
  wsConnection->lastMessage.data = NULL;
  wsConnection->lastMessage.len = 0;
  wsConnection->timeout.tv_nsec = 0;
  wsConnection->timeout.tv_sec = 0;
  wsConnection->connectionUserData = NULL;
  wsConnection->wsDesc.wsClientDesc = malloc(sizeof(struct websocket_client_desc));
  if(wsConnection->wsDesc.wsClientDesc == NULL)
  {
    goto ERROR;
  }
  memset(wsConnection->wsDesc.wsClientDesc, 0, sizeof(struct websocket_client_desc));

  wsConnection->wsDesc.wsClientDesc->wsUserData = websocketUserData;
  wsConnection->wsDesc.wsClientDesc->ws_onOpen = wsInit->ws_onOpen;
  wsConnection->wsDesc.wsClientDesc->ws_onClose = wsInit->ws_onClose;
  wsConnection->wsDesc.wsClientDesc->ws_onMessage = wsInit->ws_onMessage;
  wsConnection->wsDesc.wsClientDesc->connection = wsConnection;

  wsConnection->socketClientDesc = NULL;
  wsConnection->wsDesc.wsClientDesc->address = strdup(wsInit->address);
  if(wsConnection->wsDesc.wsClientDesc->address == NULL)
  {
    log_err("strdup failed");
    goto ERROR;
  }
  wsConnection->wsDesc.wsClientDesc->port = strdup(wsInit->port);
  if(wsConnection->wsDesc.wsClientDesc->port == NULL)
  {
    log_err("strdup failed");
    goto ERROR;
  }
  wsConnection->wsDesc.wsClientDesc->endpoint = strdup(wsInit->endpoint);
  if(wsConnection->wsDesc.wsClientDesc->endpoint == NULL)
  {
    log_err("strdup failed");
    goto ERROR;
  }

  uint32_t tempPort = strtoul(wsInit->port, NULL, 10);
  if((tempPort == 0) || (tempPort > USHRT_MAX))
  {
      log_err("port outside allowed range");
      goto ERROR;
  }

  socketInit.port = tempPort;

  socketInit.address = wsInit->address;
  socketInit.socket_onOpen = websocketClient_onOpen;
  socketInit.socket_onClose = websocket_onClose;
  socketInit.socket_onMessage = websocket_onMessage;

  wsConnection->socketClientDesc = socketClient_open(&socketInit, wsConnection);
  if(!wsConnection->socketClientDesc)
  {
    log_err("socketClient_open failed");
    goto ERROR;
  }

  socketClient_start(wsConnection->socketClientDesc);

  struct timespec timeoutStartTime;
  struct timespec currentTime;

  clock_gettime(CLOCK_MONOTONIC, &timeoutStartTime);

  while(wsConnection->state == WS_STATE_HANDSHAKE)
  {
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    if(currentTime.tv_sec > timeoutStartTime.tv_sec + MESSAGE_TIMEOUT_S)
      goto ERROR;
    usleep(10000);
  }

  return wsConnection;

  ERROR: websocketClient_close(wsConnection);
  return NULL;
}

/**
 * \brief Closes a websocket client
 *        and decreases the reference counter of wsConnectionDesc by 1
 *
 * \param *wsConnectionDesc Pointer to the websocket client descriptor
 */
void websocketClient_close(struct websocket_connection_desc *wsConnectionDesc)
{
  if(wsConnectionDesc == NULL)
    return;

  freeConnection(wsConnectionDesc);
  wsConnectionDesc->state = WS_STATE_CLOSED;
  refcnt_unref(wsConnectionDesc);
}

/**
 * \brief Returns if the client is still connected
 *
 * \param *wsConnectionDesc Pointer to the websocket connection descriptor
 *
 * \return True => connected else false
 */
bool websocketConnection_isConnected(struct websocket_connection_desc *wsConnectionDesc)
{
  return (wsConnectionDesc->state != WS_STATE_CLOSED);
}

/**
 * \brief increments the reference count of the given object
 *
 * \param *ptr Poiner to the object
 */
void websocket_ref(void *ptr)
{
  refcnt_ref(ptr);
}

/**
 * \brief Decrements the reference count of the given object and frees it if necessary
 *
 * \param *ptr Pointer to the object
 */
void websocket_unref(void *ptr)
{
  refcnt_unref(ptr);
}

/* ------------------------------ LEGACY FUNCTIONS ------------------------------ */

/**
 * \brief Opens a websocket server
 *
 * \param wsInit Pointer to the init struct
 * \param websocketUserData UserData for the socket
 *
 * \return The websocket descriptor or NULL in case of error
 *
 * \note LEGACY!!! This function is for backward compatibility
 *       it should not be used anymore but will not be
 *       removed unless there's a good reason
 */
void *websocket_open(struct websocket_init *wsInit, void *websocketUserData)
{
  struct socket_server_init socketInit;
  struct websocket_server_desc *wsDesc;

  wsDesc = refcnt_allocate(sizeof(struct websocket_server_desc), NULL);
  if(!wsDesc)
  {
    log_err("refcnt_allocate failed");
    return NULL;
  }
  memset(wsDesc, 0, sizeof(struct websocket_server_desc));

  wsDesc->ws_onOpenLegacy = (void*(*)(struct websocket_server_desc *, struct websocket_connection_desc *)) wsInit->ws_onOpen;
  wsDesc->ws_onCloseLegacy = (void (*)(void*, struct websocket_connection_desc*, void*))wsInit->ws_onClose;
  wsDesc->ws_onClose = NULL;
  wsDesc->ws_onMessage =
      (void (*)(void*, struct websocket_connection_desc*, void*, enum ws_data_type, void*, size_t))wsInit->ws_onMessage;
  wsDesc->wsSocketUserData = websocketUserData;

  socketInit.address = wsInit->address;
  socketInit.port = wsInit->port;
  socketInit.socket_onOpen = websocketServer_onOpen;
  socketInit.socket_onClose = websocket_onClose;
  socketInit.socket_onMessage = websocket_onMessage;

  wsDesc->socketDesc = socketServer_open(&socketInit, wsDesc);
  if(!wsDesc->socketDesc)
  {
    log_err("socketServer_open failed");
    refcnt_unref(wsDesc);
    return NULL;
  }

  return wsDesc;
}

/**
 * \brief Closes the given websocket server
 *
 * \param *wsDesc Pointer to the websocket descriptor
 *
 * \note LEGACY!!! This function is for backward compatibility
 *       it should not be used anymore but will not be
 *       removed unless there's a good reason
 */
void websocket_close(void *wsDesc)
{
  websocketServer_close(wsDesc);
}

/**
 * \brief Returns the user data of the given connection
 *
 * \param *wsClientDesc Pointer to the websocket client descriptor
 *
 * \note LEGACY!!! This function is for backward compatibility
 *       it should not be used anymore but will not be
 *       removed unless there's a good reason
 */
void *websocket_getClientUserData(void *wsClientDesc)
{
  return websocket_getConnectionUserData(wsClientDesc);
}
