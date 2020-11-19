/*
 * websocket.c
 *
 *  Created on: Mar 23, 2017
 *      Author: Clemens Kresser
 *      License: MIT
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

struct websocket_desc
{
  //! callback that is called when a message is received on the websocket
  void (*ws_onMessage)(void *socketUserData, void *sessionDesc, void *sessionUserData, enum ws_data_type dataType,
                       void *msg, size_t len);
  //! callback that is called when the websocket is connected
  void* (*ws_onOpen)(void *wsDesc, void *sessionDesc);
  //! callback that is called when the websocket is closed
  void (*ws_onClose)(void *socketUserData, void *sessionDesc, void *sessionUserData);
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

struct websocket_session_desc
{
  //! indicates if it is a websocket client or a websocket server
  enum ws_type wsType;
  //! pointer to the socket client descriptor
  void *socketClientDesc;
  //! the connection state of the websocket (handshake, connected, closed)
  volatile enum ws_state state;
  //! information about the last received message
  struct last_message lastMessage;
  //! pointer to the session user data
  void *sessionUserData;
  //! stores the time for message timeouts
  struct timespec timeout;
  union
  {
    //!pointer to the websocket client descriptor (in case of client mode)
    struct websocket_client_desc *wsClientDesc;
    //!pointer to the websocket server descriptor (in case of server mode)
    struct websocket_desc *wsServerDesc;
  } wsDesc;
};

struct websocket_client_desc
{
  //! callback that is called when a message is received on the websocket
  void (*ws_onMessage)(void *socketUserData, void *sessionDesc, void *sessionUserData, enum ws_data_type dataType,
                       void *msg, size_t len);
  //! callback that is called when the websocket is connected
  void* (*ws_onOpen)(void *socketUserData, void *wsDesc, void *sessionDesc);
  //! callback that is called when the websocket is closed
  void (*ws_onClose)(void *socketUserData, void *sessionDesc, void *sessionUserData);
  //! pointer to the socket descriptor
  void *socketDesc;
  //! pointer to the user data
  void *wsUserData;
  //! the websocket session descriptor for the current connection
  struct websocket_session_desc session;
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
 * \brief: calculates the Sec-WebSocket-Accept from the Sec-WebSocket-Key
 *
 * \param *key: pointer to the key that should be used
 *
 * \return: to the string containing the Sec-WebSocket-Accept (must be freed after use)
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
 * \brief: parses the http header and extracts the Sec-WebSocket-Key
 *
 * \param *wsHeader: pointer to the string with the header
 * \param len: length of the header
 * \param *key: pointer to where the key should be stored (should be at least WS_HS_KEY_LEN big)
 *
 * \return: 0 if successful else -1
 *
 */
static int parseHttpHeader(char *wsHeader, size_t len, char *key)
{
  char *cpnt;
  int i;

  cpnt = strnstr(wsHeader, WS_HS_KEY_ID, len);
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

  for(i = 0; (i < WS_HS_KEY_LEN - 1) && isgraph(cpnt[i]) && ((&cpnt[i] - wsHeader) < len); i++)
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
 * \brief: sends the websocket handshake reply
 *
 * \param *socketClientDesc: the client descriptor of the socket
 * \param *replyKey: the calculated Sec-WebSocket-Accept key
 *
 * \return: -1 on error 0 if successful
 *
 */
static int sendWsHandshakeReply(void *socketClientDesc, char *replyKey)
{
  char replyHeader[strlen(WS_HANDSHAKE_REPLY_BLUEPRINT) + 28];

  if(snprintf(replyHeader, sizeof(replyHeader), WS_HANDSHAKE_REPLY_BLUEPRINT, replyKey) >= sizeof(replyHeader))
  {
    log_err("problem with the handshake reply key (buffer to small)");
    return -1;
  }

  return socketServer_send(socketClientDesc, replyHeader, strlen(replyHeader));
}

#define WS_HS_REPLY_ID "Sec-WebSocket-Accept:"


/**
 * \brief checks if we've received the correct handshake reply
 *
 * \param *websocketDesc Pointer to the websocket descriptor
 * \param *header Pointer to the header
 * \param [in-out]len input the length of the message output the length of the header
 *
 * \return true => handshake correct else false
 */
static bool checkWsHandshakeReply(struct websocket_session_desc *wsSessionDesc, char *header, size_t *len)
{
  if(wsSessionDesc->wsType == WS_TYPE_SERVER)
    return false;

  struct websocket_client_desc *wsDesc = wsSessionDesc->wsDesc.wsClientDesc;

  char *cpnt;
  int i;
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

  for(i = 0; (i < sizeof(key) - 1) && isgraph(cpnt[i]) && ((&cpnt[i] - header) < *len); i++)
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
 * \brief sends the websocket handshake request
 *
 * \param *wsDesc Pointer to the websocket descriptor
 *
 * \return true if successful else false
 */
static bool sendWsHandshakeRequest(struct websocket_client_desc *wsDesc)
{
  unsigned char wsKeyBytes[16];
  int i;
  char *requestHeader = NULL;
  bool success = false;

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

  if(socketClient_send(wsDesc->socketDesc, requestHeader, strlen(requestHeader)) == -1)
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
 * \brief: prints the websocket header (for debugging purpose only)
 *
 * \param *header: pointer to the header
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
 * \brief: parses the header of a websocket message
 *
 * \param *data: pointer to the message received from the socket
 * \param len: the length of the data
 * \param *header: pointer to where the header should be written to
 *
 * \return: 1 if successful 0 if msg to short else -1
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
  int i, lengthNumBytes;

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
 * \brief: creates the websocket header from the given variables
 *
 * \param[out] *buffer: pointer to the buffer where the header should be written to
 *                 should be able to hold at least 10 bytes
 * \param opcode: the opcode that should be used for the header
 * \param fin: fin bit (is this the last frame (true) or will more follow (false))
 * \param masked: true => add a mask, false => add no mask
 * \param mask: the mask that should be used (ignored in case masked is false)
 * \param len: the length of the payload
 *
 * \return: the length of the header
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
 * \brief copies the data in the buffer pointed by from to the buffer pointed by to and mask
 *        them
 *
 * \param *to pointer to where the data should copied masked
 * \param *from pointer to the original data
 * \param mask the mask that shoud be used (32-bit)
 * \param len the length of the data that should be copied
 *
 * \note the masking algorithm is big endian XOR
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
 * \brief: sends data through websockets with custom opcodes
 *
 * \param *wsSessionDesc: pointer to the websocket session descriptor
 * \param opcode: the opcode to use
 * \param fin: true if this is the last frame of a sequence else false
 * \param masked: true => send masked (client to server) else false (server to client)
 * \param *msg: the payload data
 * \param len: the payload length
 *
 * \return 0 if successful else -1
 */
static int sendDataLowLevel(void *wsSessionDesc, enum ws_opcode opcode, bool fin, bool masked, const void *msg,
                            size_t len)
{
  struct websocket_session_desc *websockSessionDesc = wsSessionDesc;
  unsigned char header[14];  //the maximum size of a websocket header is 14
  int headerLength;
  unsigned char *sendBuffer;
  int rc = -1;
  unsigned long mask = 0;

  if(websockSessionDesc->state == WS_STATE_CLOSED)
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

  switch(websockSessionDesc->wsType)
  {
    case WS_TYPE_SERVER:
      rc = socketServer_send(websockSessionDesc->socketClientDesc, sendBuffer, len + headerLength);
      break;

    case WS_TYPE_CLIENT:
      rc = socketClient_send(websockSessionDesc->wsDesc.wsClientDesc->socketDesc, sendBuffer, len + headerLength);
      break;
  }
  free(sendBuffer);

#ifdef VERBOSE_MODE
  log_dbg("%s retv:%d", __func__, rc);
#endif

  return rc;
}

/**
 * \brief: checks if the given close code is valid
 *
 * \param code: the code that should be checked
 *
 * \return: true if code is valid else false
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
 * \brief: handles the first message (which is sometimes followed by a cont message)
 *
 * \param *wsSessionDesc: pointer to the websocket session descriptor
 * \param *data: pointer to the payload data
 * \param len: the length of the payload data
 * \param *header: pointer to the parsed websocket header structure
 *
 * \return: the message state
 */
static enum ws_msg_state handleFirstMessage(struct websocket_session_desc *wsSessionDesc, const unsigned char *data,
                                            size_t len, struct ws_header *header)
{
  size_t i;

  if(!header->masked && (wsSessionDesc->wsType == WS_TYPE_SERVER))
  {
    websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(wsSessionDesc->lastMessage.data != NULL)
  {
    log_err("last message not finished");
    websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(header->payloadLength) //it's allowed to send frames with payload length = 0
  {
    if(header->fin)
      wsSessionDesc->lastMessage.data = refcnt_allocate(header->payloadLength, NULL);
    else
      wsSessionDesc->lastMessage.data = malloc(header->payloadLength);
    if(!wsSessionDesc->lastMessage.data)
    {
      log_err("refcnt_allocate failed dropping message");
      return WS_MSG_STATE_ERROR;
    }

    if(header->masked)
    {
      for(i = 0; i < header->payloadLength; i++)
      {
        wsSessionDesc->lastMessage.data[i] = data[header->payloadStartOffset + i] ^ header->mask[i % 4];
      }
    }
    else
    {
      memcpy(wsSessionDesc->lastMessage.data, &data[header->payloadStartOffset], header->payloadLength);
    }
  }

  wsSessionDesc->lastMessage.firstReceived = true;
  wsSessionDesc->lastMessage.complete = header->fin;
  wsSessionDesc->lastMessage.dataType = header->opcode == WS_OPCODE_TEXT ? WS_DATA_TYPE_TEXT : WS_DATA_TYPE_BINARY;

  wsSessionDesc->lastMessage.len = header->payloadLength;
  if(wsSessionDesc->lastMessage.dataType == WS_DATA_TYPE_TEXT)
  {
    enum utf8_state state;

    wsSessionDesc->lastMessage.utf8Handle = 0;
    state = utf8_validate(wsSessionDesc->lastMessage.data, wsSessionDesc->lastMessage.len,
        &wsSessionDesc->lastMessage.utf8Handle);
    if((header->fin && (state != UTF8_STATE_OK)) || (!header->fin && (state == UTF8_STATE_FAIL)))
    {
      log_err("no valid utf8 string closing connection");
      websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_INVALID_DATA);
      return WS_MSG_STATE_ERROR;
    }
  }
  if(header->fin)
    return WS_MSG_STATE_USER_DATA;
  else
    return WS_MSG_STATE_NO_USER_DATA;
}

/**
 * \brief: handles a cont message
 *
 * \param *wsSessionDesc: pointer to the websocket session descriptor
 * \param *data: pointer to the payload data
 * \param len: the length of the payload data
 * \param *header: pointer to the parsed websocket header structure
 *
 * \return: the message state
 */
static enum ws_msg_state handleContMessage(struct websocket_session_desc *wsSessionDesc, const unsigned char *data,
                                           size_t len, struct ws_header *header)
{
  size_t i;
  char *temp;

  if(!wsSessionDesc->lastMessage.firstReceived)
  {
    log_err("missing last message closing connection");
    websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if((wsSessionDesc->wsType != WS_TYPE_SERVER) == header->masked)
  {
    log_err("mask bit wrong");
    websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(wsSessionDesc->lastMessage.len + header->payloadLength) //it's allowed to send frames with payload length = 0
  {
    if(header->fin)
    {
      temp = refcnt_allocate(wsSessionDesc->lastMessage.len + header->payloadLength, NULL);
      if(!temp)
      {
        log_err("refcnt_allocate failed dropping message");
        free(wsSessionDesc->lastMessage.data);
        wsSessionDesc->lastMessage.data = NULL;
        return WS_MSG_STATE_ERROR;
      }
      memcpy(temp, wsSessionDesc->lastMessage.data, wsSessionDesc->lastMessage.len);
      free(wsSessionDesc->lastMessage.data);
      wsSessionDesc->lastMessage.data = temp;
    }
    else
    {
      temp = realloc(wsSessionDesc->lastMessage.data, wsSessionDesc->lastMessage.len + header->payloadLength);
      if(!temp)
      {
        log_err("realloc failed dropping message");
        free(wsSessionDesc->lastMessage.data);
        wsSessionDesc->lastMessage.data = NULL;
        return WS_MSG_STATE_ERROR;
      }
      wsSessionDesc->lastMessage.data = temp;
    }

    if(header->masked)
    {
      for(i = 0; i < header->payloadLength; i++)
      {
        wsSessionDesc->lastMessage.data[wsSessionDesc->lastMessage.len + i] = data[header->payloadStartOffset + i]
            ^ header->mask[i % 4];
      }
    }
    else
    {
      memcpy(&wsSessionDesc->lastMessage.data[wsSessionDesc->lastMessage.len], &data[header->payloadStartOffset],
          header->payloadLength);
    }
  }
  wsSessionDesc->lastMessage.complete = header->fin;

  if(wsSessionDesc->lastMessage.dataType == WS_DATA_TYPE_TEXT)
  {
    enum utf8_state state;
    state = utf8_validate(&wsSessionDesc->lastMessage.data[wsSessionDesc->lastMessage.len], header->payloadLength,
        &wsSessionDesc->lastMessage.utf8Handle);
    if((header->fin && state != UTF8_STATE_OK) || (!header->fin && state == UTF8_STATE_FAIL))
    {
      log_err("no valid utf8 string closing connection");
      websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_INVALID_DATA);
      return WS_MSG_STATE_ERROR;
    }
  }
  wsSessionDesc->lastMessage.len += header->payloadLength;

  if(header->fin)
    return WS_MSG_STATE_USER_DATA;
  else
    return WS_MSG_STATE_NO_USER_DATA;
}

/**
 * \brief: handles a ping message and replies with a pong message
 *
 * \param *wsSessionDesc: pointer to the websocket client descriptor
 * \param *data: pointer to the payload data
 * \param len: the length of the payload data
 * \param *header: pointer to the parsed websocket header structure
 *
 * \return: the message state
 */
static enum ws_msg_state handlePingMessage(struct websocket_session_desc *wsSessionDesc, const unsigned char *data,
                                           size_t len, struct ws_header *header)
{
  int rc;
  char *temp;
  size_t i;
  bool masked = (wsSessionDesc->wsType == WS_TYPE_CLIENT);

  if(header->fin)
  {
    if(header->payloadLength > MAX_DEFAULT_PAYLOAD_LENGTH)
    {
      websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
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

      if(sendDataLowLevel(wsSessionDesc, WS_OPCODE_PONG, true, masked, temp, header->payloadLength) == 0)
        rc = WS_MSG_STATE_NO_USER_DATA;
      else
        rc = WS_MSG_STATE_ERROR;
      free(temp);
      return rc;
    }
    else
    {
      if(sendDataLowLevel(wsSessionDesc, WS_OPCODE_PONG, true, masked, &data[header->payloadStartOffset],
          header->payloadLength) == 0)
        return WS_MSG_STATE_NO_USER_DATA;
      else
        return WS_MSG_STATE_ERROR;
    }
  }
  else
  {
    websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }
}

/**
 * \brief: handles a pong message
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param *data: pointer to the payload data
 * \param len: the length of the payload data
 * \param *header: pointer to the parsed websocket header structure
 *
 * \return: the message state
 */
static enum ws_msg_state handlePongMessage(struct websocket_session_desc *wsClientDesc, const unsigned char *data,
                                           size_t len, struct ws_header *header)
{
  if(header->fin && (header->payloadLength <= MAX_DEFAULT_PAYLOAD_LENGTH))
  {
    //Pongs are ignored now because actually we also don't send pings
    return WS_MSG_STATE_NO_USER_DATA;
  }
  else
  {
    websocket_closeSession(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }
}

/**
 * \brief: handles a disconnect message and replies with a disconnect message
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param *data: pointer to the payload data
 * \param len: the length of the payload data
 * \param *header: pointer to the parsed websocket header structure
 *
 * \return: the message state
 */
static enum ws_msg_state handleDisconnectMessage(struct websocket_session_desc *wsSessionDesc,
                                                 const unsigned char *data, size_t len, struct ws_header *header)
{
  size_t i;
  int rc;
  unsigned long utf8Handle = 0;
  bool masked = (wsSessionDesc->wsType == WS_TYPE_CLIENT);

  if((header->fin) && (header->payloadLength != 1) && (header->payloadLength <= MAX_DEFAULT_PAYLOAD_LENGTH))
  {
    enum ws_close_code code;

    if(!header->payloadLength)
    {
      websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_NORMAL);
      return WS_MSG_STATE_NO_USER_DATA;
    }
    else if(header->masked == (wsSessionDesc->wsType == WS_TYPE_SERVER))
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
        websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
        return WS_MSG_STATE_ERROR;
      }
      else if((header->payloadLength == 2)
          || (UTF8_STATE_OK == utf8_validate(&tempBuffer[2], header->payloadLength - 2, &utf8Handle)))
      {
        websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_NORMAL);
        return WS_MSG_STATE_NO_USER_DATA;
      }
      else
      {
        websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_INVALID_DATA);
        return WS_MSG_STATE_ERROR;
      }
    }
    else
    {
      if((header->payloadLength == 2)
          || utf8_validate((char*)&data[header->payloadStartOffset + 2], header->payloadLength - 2, &utf8Handle))
      {
        if(sendDataLowLevel(wsSessionDesc, WS_OPCODE_DISCONNECT, true, masked, &data[header->payloadStartOffset],
            header->payloadLength) == 0)
          rc = WS_MSG_STATE_NO_USER_DATA;
        else
          rc = WS_MSG_STATE_ERROR;
        switch(wsSessionDesc->wsType)
        {
          case WS_TYPE_SERVER:
            socketServer_closeClient(wsSessionDesc->socketClientDesc);
            break;

          case WS_TYPE_CLIENT:
            socketClient_closeConnection(wsSessionDesc->wsDesc.wsClientDesc->socketDesc);
            break;
        }
      }
      else
      {
        websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_INVALID_DATA);
        rc = WS_MSG_STATE_ERROR;
      }
      return rc;
    }
  }
  else
  {
    websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }
}

/**
 * \brief: parses a message and stores it to the client descriptor
 *
 * \param *wsSessionDesc: pointer to the websocket session descriptor
 * \param *data: pointer to the received data
 * \param len: the length of the data
 * \param header: the parsed header struct (as parsed by parseWebsocketHeader)
 *
 * \return one of ws_msg_state
 */
static enum ws_msg_state parseMessage(struct websocket_session_desc *wsSessionDesc, const unsigned char *data,
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
      return handleFirstMessage(wsSessionDesc, data, len, header);

    case WS_OPCODE_CONTINUATION:
      return handleContMessage(wsSessionDesc, data, len, header);

    case WS_OPCODE_PING:
      return handlePingMessage(wsSessionDesc, data, len, header);

    case WS_OPCODE_PONG:
      return handlePongMessage(wsSessionDesc, data, len, header);

    case WS_OPCODE_DISCONNECT:
      return handleDisconnectMessage(wsSessionDesc, data, len, header);

    default:
      log_err("unknown opcode (%d)", header->opcode);
      rc = WS_MSG_STATE_ERROR;
      break;
  }

  return rc;
}

/**
 * \brief: function that gets called when a connection to a client is established
 *         allocates and initialises the wsClientDesc
 *
 * \param *socketUserData: in this case this is the websocket descriptor
 * \param *socketClientDesc: the client descriptor from the socket server
 *
 * \return: pointer to the websocket session descriptor
 */
static void* websocket_onOpen(void *socketUserData, void *socketClientDesc)
{
  struct websocket_desc *wsDesc = socketUserData;
  struct websocket_session_desc *wsSessionDesc;

  if(wsDesc == NULL)
  {
    log_err("%s(): wsDesc must not be NULL!", __func__);
    return NULL;
  }

  if(socketClientDesc == NULL)
  {
    log_err("%s(): socketClientDesc must not be NULL!", __func__);
    return NULL;
  }

  refcnt_ref(socketClientDesc);
  wsSessionDesc = refcnt_allocate(sizeof(struct websocket_session_desc), NULL);
  wsSessionDesc->wsType = WS_TYPE_SERVER;
  wsSessionDesc->socketClientDesc = socketClientDesc;
  wsSessionDesc->state = WS_STATE_HANDSHAKE;
  wsSessionDesc->timeout.tv_nsec = 0;
  wsSessionDesc->timeout.tv_sec = 0;
  wsSessionDesc->lastMessage.data = NULL;
  wsSessionDesc->lastMessage.len = 0;
  wsSessionDesc->lastMessage.complete = false;
  wsSessionDesc->wsDesc.wsServerDesc = wsDesc;

  if(wsDesc->ws_onOpen)
    wsSessionDesc->sessionUserData = wsDesc->ws_onOpen(wsDesc, wsSessionDesc);

  return wsSessionDesc;
}

/**
 * \brief function that get's called when a websocket client connection is
 *        established
 *
 * \param *socketUserData In this case this is the websocket descriptor
 * \param *socketDesc The socket descriptor of the socket client
 */
static void* websocketClient_onOpen(void *socketUserData, void *socketDesc)
{
  struct websocket_client_desc *wsDesc = socketUserData;

  if(wsDesc->ws_onOpen)
    wsDesc->session.sessionUserData = wsDesc->ws_onOpen( wsDesc->wsUserData, wsDesc, &wsDesc->session);
  else
    wsDesc->session.sessionUserData = NULL;

  if(!sendWsHandshakeRequest(wsDesc))
  {
    if(wsDesc->ws_onClose != NULL)
      wsDesc->ws_onClose(wsDesc->wsUserData, &wsDesc->session, wsDesc->session.sessionUserData);
    wsDesc->session.state = WS_STATE_CLOSED;
  }

  return &wsDesc->session;
}

/**
 * \brief gets called when the websocket is closed
 *
 * \param *wsSessionDesc pointer to the websocket session descriptor
 */
static void callOnClose(struct websocket_session_desc *wsSessionDesc)
{
  switch(wsSessionDesc->wsType)
  {
    case WS_TYPE_SERVER:
      if(wsSessionDesc->wsDesc.wsServerDesc->ws_onClose != NULL)
        wsSessionDesc->wsDesc.wsServerDesc->ws_onClose(wsSessionDesc->wsDesc.wsServerDesc->wsSocketUserData,
            wsSessionDesc, wsSessionDesc->sessionUserData);
      break;

    case WS_TYPE_CLIENT:
      if(wsSessionDesc->wsDesc.wsClientDesc->ws_onClose != NULL)
        wsSessionDesc->wsDesc.wsClientDesc->ws_onClose(wsSessionDesc->wsDesc.wsClientDesc->wsUserData, wsSessionDesc,
            wsSessionDesc->sessionUserData);
      break;
  }
}

/**
 * \brief: function that gets called when a connection to a client is closed
 *         frees the websocket client descriptor
 *
 * \param *socketUserData: in this case this is the websocket descriptor
 * \param *socketClientDesc: the client descriptor from the socket server
 * \param *sessionDescriptor: in this case this is the websocket session descriptor
 *
 */
static void websocket_onClose(void *socketUserData, void *socketClientDesc, void *sessionDescriptor)
{
  struct websocket_desc *wsDesc = socketUserData;
  struct websocket_session_desc *wsSessionDesc = sessionDescriptor;

  if(wsDesc == NULL)
  {
    log_err("%s(): wsDesc must not be NULL!", __func__);
    return;
  }

  if(wsSessionDesc == NULL)
  {
    log_err("%s(): wsSessionDesc must not be NULL!", __func__);
    return;
  }

  if(wsSessionDesc->lastMessage.data && wsSessionDesc->lastMessage.complete)
    refcnt_unref(wsSessionDesc->lastMessage.data);
  else
    free(wsSessionDesc->lastMessage.data);
  wsSessionDesc->lastMessage.data = NULL;

  if(wsSessionDesc->state != WS_STATE_CLOSED)
  {
    wsSessionDesc->state = WS_STATE_CLOSED;

    callOnClose(wsSessionDesc);
  }

  if(wsSessionDesc->wsType == WS_TYPE_SERVER) //in client mode the session descriptor is not allocated
  {
    if(wsSessionDesc->socketClientDesc)
      refcnt_unref(wsSessionDesc->socketClientDesc);
    wsSessionDesc->socketClientDesc = NULL;
    refcnt_unref(wsSessionDesc);
  }
}

/**
 * \brief gets called when a message was received on the websocket
 *
 * \param *wsSessionDesc pointer to the websocket session descriptor
 */
static void callOnMessage(struct websocket_session_desc *wsSessionDesc)
{
  switch(wsSessionDesc->wsType)
  {
    case WS_TYPE_SERVER:
    {
      if(wsSessionDesc->wsDesc.wsServerDesc->ws_onMessage)
      {
        wsSessionDesc->wsDesc.wsServerDesc->ws_onMessage(wsSessionDesc->wsDesc.wsServerDesc->wsSocketUserData,
            wsSessionDesc, wsSessionDesc->sessionUserData, wsSessionDesc->lastMessage.dataType,
            wsSessionDesc->lastMessage.data, wsSessionDesc->lastMessage.len);
      }
    }
      break;

    case WS_TYPE_CLIENT:
    {
      if(wsSessionDesc->wsDesc.wsClientDesc->ws_onMessage)
      {
        wsSessionDesc->wsDesc.wsClientDesc->ws_onMessage(wsSessionDesc->wsDesc.wsServerDesc->wsSocketUserData,
            wsSessionDesc, wsSessionDesc->sessionUserData, wsSessionDesc->lastMessage.dataType,
            wsSessionDesc->lastMessage.data, wsSessionDesc->lastMessage.len);
      }
    }
      break;
  }
}

/**
 * \brief: function that gets called when a message arrives at the socket server
 *
 * \param *socketUserData: in this case this is the websocket descriptor
 * \param *socketClientDesc: the client descriptor from the socket server
 * \param *sessionDescriptor: in this case this is the websocket session descriptor
 * \param *msg: pointer to the buffer containing the data
 * \param len: the length of msg
 *
 * \return: the amount of bytes read
 *
 */
static size_t websocket_onMessage(void *socketUserData, void *socketClientDesc, void *sessionDescriptor, void *msg, size_t len)
{
  struct websocket_desc *wsDesc = socketUserData;
  struct websocket_session_desc *wsSessionDesc = sessionDescriptor;
  struct ws_header wsHeader = {0};
  struct timespec now;

  char key[WS_HS_KEY_LEN];
  char *replyKey;

  if(wsDesc == NULL)
  {
    log_err("%s(): wsDesc must not be NULL!", __func__);
    return 0;
  }

  if(wsSessionDesc == NULL)
  {
    log_err("%s(): wsClientDesc must not be NULL!", __func__);
    return 0;
  }

  if(socketClientDesc == NULL)
  {
    log_err("%s(): socketClientDesc must not be NULL!", __func__);
    return 0;
  }

  switch(wsSessionDesc->state)
  {
    case WS_STATE_HANDSHAKE:
      switch(wsSessionDesc->wsType)
      {
        case WS_TYPE_SERVER:
          if(parseHttpHeader(msg, len, key) == 0)
          {
            replyKey = calculateSecWebSocketAccept(key);
            if(replyKey == NULL)
            {
              log_err("%s(): calculateSecWebSocketAccept failed!", __func__);
              return 0;
            }

#ifdef VERBOSE_MODE
                  log_dbg("%s() replyKey:%s", __func__, replyKey);
#endif

            sendWsHandshakeReply(socketClientDesc, replyKey);

            free(replyKey);
            wsSessionDesc->state = WS_STATE_CONNECTED;
          }
          else
          {
            log_err("parseHttpHeader failed");
          }
          break;

        case WS_TYPE_CLIENT:
          if(checkWsHandshakeReply(wsSessionDesc, msg, &len))
          {
            wsSessionDesc->state = WS_STATE_CONNECTED;
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
          websocket_closeSession(wsSessionDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
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

      switch(parseMessage(wsSessionDesc, msg, len, &wsHeader))
      {
        case WS_MSG_STATE_NO_USER_DATA:
          wsSessionDesc->timeout.tv_nsec = 0;
          wsSessionDesc->timeout.tv_sec = 0;
          return wsHeader.payloadLength + wsHeader.payloadStartOffset;

        case WS_MSG_STATE_USER_DATA:
          callOnMessage(wsSessionDesc);
          if(wsSessionDesc->lastMessage.data)
            refcnt_unref(wsSessionDesc->lastMessage.data);
          wsSessionDesc->lastMessage.data = NULL;
          wsSessionDesc->lastMessage.complete = false;
          wsSessionDesc->lastMessage.firstReceived = false;
          wsSessionDesc->lastMessage.len = 0;
          wsSessionDesc->timeout.tv_nsec = 0;
          wsSessionDesc->timeout.tv_sec = 0;
          return wsHeader.payloadLength + wsHeader.payloadStartOffset;

        case WS_MSG_STATE_INCOMPLETE:
          clock_gettime(CLOCK_MONOTONIC, &now);
          if(wsSessionDesc->timeout.tv_sec == (wsSessionDesc->timeout.tv_nsec == 0))
          {
            wsSessionDesc->timeout = now;
          }
          else if(wsSessionDesc->timeout.tv_sec > now.tv_sec + MESSAGE_TIMEOUT_S)
          {
            free(wsSessionDesc->lastMessage.data);
            wsSessionDesc->lastMessage.data = NULL;
            wsSessionDesc->lastMessage.len = 0;
            wsSessionDesc->lastMessage.complete = 0;
            wsSessionDesc->timeout.tv_sec = 0;
            wsSessionDesc->timeout.tv_nsec = 0;
            log_err("message timeout");
            return len;
          }
          return 0;

        case WS_MSG_STATE_ERROR:
          if(wsSessionDesc->lastMessage.data && wsSessionDesc->lastMessage.complete)
            refcnt_unref(wsSessionDesc->lastMessage.data);
          else
            free(wsSessionDesc->lastMessage.data);
          wsSessionDesc->lastMessage.data = NULL;
          wsSessionDesc->lastMessage.len = 0;
          wsSessionDesc->lastMessage.complete = 0;
          wsSessionDesc->timeout.tv_sec = 0;
          wsSessionDesc->timeout.tv_nsec = 0;
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
int websocket_sendData(void *wsSessionDescriptor, enum ws_data_type dataType, const void *msg, size_t len)
{
  unsigned char opcode;
  struct websocket_session_desc *wsSessionDesc = wsSessionDescriptor;
  bool masked = (wsSessionDesc->wsType == WS_TYPE_CLIENT);

  if(wsSessionDesc->state != WS_STATE_CONNECTED)
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

  return sendDataLowLevel(wsSessionDesc, opcode, true, masked, msg, len);
}

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
int websocket_sendDataFragmentedStart(void *wsSessionDescriptor, enum ws_data_type dataType, const void *msg,
                                      size_t len)
{
  unsigned char opcode;
  struct websocket_session_desc *wsSessionDesc = wsSessionDescriptor;
  bool masked = (wsSessionDesc->wsType == WS_TYPE_CLIENT);

  if(wsSessionDesc->state != WS_STATE_CONNECTED)
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

  return sendDataLowLevel(wsSessionDesc, opcode, false, masked, msg, len);
}

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
int websocket_sendDataFragmentedCont(void *wsSessionDescriptor, bool fin, const void *msg, size_t len)
{
  struct websocket_session_desc *wsSessionDesc = wsSessionDescriptor;
  bool masked = (wsSessionDesc->wsType == WS_TYPE_CLIENT);

  if(wsSessionDesc->state != WS_STATE_CONNECTED)
    return -1;

  return sendDataLowLevel(wsSessionDesc, WS_OPCODE_CONTINUATION, fin, masked, msg, len);
}

/**
 * \brief: closes the given websocket session
 *
 * \param *wsSessionDesc: pointer to the websocket session descriptor
 * \param code: the closing code
 */
void websocket_closeSession(void *wsSessionDescriptor, enum ws_close_code code)
{
  struct websocket_session_desc *wsSessionDesc = wsSessionDescriptor;
  unsigned char help[2];
  bool masked = (wsSessionDesc->wsType == WS_TYPE_CLIENT);

  help[0] = (unsigned long)code >> 8;
  help[1] = (unsigned long)code & 0xFF;

  sendDataLowLevel(wsSessionDesc, WS_OPCODE_DISCONNECT, true, masked, help, 2);

  if(wsSessionDesc->lastMessage.data && wsSessionDesc->lastMessage.complete)
    refcnt_unref(wsSessionDesc->lastMessage.data);
  else
    free(wsSessionDesc->lastMessage.data);

  wsSessionDesc->lastMessage.data = NULL;
  wsSessionDesc->lastMessage.len = 0;
  wsSessionDesc->lastMessage.complete = 0;

  switch(wsSessionDesc->wsType)
  {
    case WS_TYPE_SERVER:
      socketServer_closeClient(wsSessionDesc->socketClientDesc);
      break;

    case WS_TYPE_CLIENT:
      socketClient_closeConnection(wsSessionDesc->socketClientDesc);
      break;
  }
}

/**
 * \brief: returns the user data of the given client
 *
 * \param *wsSessionDesc: pointer to the websocket client descriptor
 *
 */
void* websocket_getClientUserData(void *wsSessionDesc)
{
  struct websocket_session_desc *wsSessionDescriptor = wsSessionDesc;

  return wsSessionDescriptor->sessionUserData;
}

/**
 * \brief: opens a websocket server
 *
 * \param wsInit: pointer to the init struct
 * \param websocketUserData: userData for the socket
 *
 * \return: the websocket descriptor or NULL in case of error
 */
void* websocketServer_open(struct websocket_server_init *wsInit, void *websocketUserData)
{
  struct socket_init socketInit;
  struct websocket_desc *wsDesc;

  wsDesc = refcnt_allocate(sizeof(struct websocket_desc), NULL);
  if(!wsDesc)
  {
    log_err("refcnt_allocate failed");
    return NULL;
  }

  wsDesc->ws_onOpen = wsInit->ws_onOpen;
  wsDesc->ws_onClose = wsInit->ws_onClose;
  wsDesc->ws_onMessage = wsInit->ws_onMessage;
  wsDesc->wsSocketUserData = websocketUserData;

  socketInit.address = wsInit->address;
  socketInit.port = wsInit->port;
  socketInit.socket_onOpen = websocket_onOpen;
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
 * \brief opens a websocket client connection
 *
 * \param wsInit: pointer to the init struct
 * \param websocketUserData: userData for the socket
 *
 * \return: the websocket client descriptor or NULL in case of error
 */
void* websocketClient_open(struct websocket_client_init *wsInit, void *websocketUserData)
{
  struct socket_client_init socketInit;
  struct websocket_client_desc *wsDesc;

  wsDesc = refcnt_allocate(sizeof(struct websocket_client_desc), NULL);
  if(wsDesc == NULL)
  {
    log_err("refcnt_allocate failed");
    return NULL;
  }

  memset(wsDesc, 0, sizeof(struct websocket_client_desc));

  wsDesc->socketDesc = NULL;
  wsDesc->wsUserData = websocketUserData;
  wsDesc->ws_onOpen = wsInit->ws_onOpen;
  wsDesc->ws_onClose = wsInit->ws_onClose;
  wsDesc->ws_onMessage = wsInit->ws_onMessage;
  wsDesc->session.wsType = WS_TYPE_CLIENT;
  wsDesc->session.state = WS_STATE_HANDSHAKE;
  wsDesc->session.lastMessage.firstReceived = false;
  wsDesc->session.lastMessage.complete = false;
  wsDesc->session.lastMessage.data = NULL;
  wsDesc->session.lastMessage.len = 0;
  wsDesc->session.timeout.tv_nsec = 0;
  wsDesc->session.timeout.tv_sec = 0;
  wsDesc->session.sessionUserData = NULL; //unused in client mode because it's the same as socket user data
  wsDesc->session.wsDesc.wsClientDesc = wsDesc; //the session should know it's parent
  wsDesc->session.socketClientDesc = NULL;
  wsDesc->address = strdup(wsInit->address);
  if(wsDesc->address == NULL)
  {
    log_err("strdup failed");
    goto ERROR;
  }
  wsDesc->port = strdup(wsInit->port);
  if(wsDesc->port == NULL)
  {
    log_err("strdup failed");
    goto ERROR;
  }
  wsDesc->endpoint = strdup(wsInit->endpoint);
  if(wsDesc->address == NULL)
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

  wsDesc->session.socketClientDesc = socketClient_open(&socketInit, wsDesc);
  if(!wsDesc->session.socketClientDesc)
  {
    log_err("socketServer_open failed");
    goto ERROR;
  }
  wsDesc->socketDesc = wsDesc->session.socketClientDesc;

  socketClient_start(wsDesc->socketDesc);

  struct timespec timeoutStartTime;
  struct timespec currentTime;

  clock_gettime(CLOCK_MONOTONIC, &timeoutStartTime);

  while(wsDesc->session.state == WS_STATE_HANDSHAKE)
  {
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    if(currentTime.tv_sec > timeoutStartTime.tv_sec + MESSAGE_TIMEOUT_S)
      goto ERROR;
    usleep(10000);
  }

  return &wsDesc->session;

  ERROR: websocketClient_close(&wsDesc->session);
  return NULL;
}

/**
 * \brief closes a websocket client
 *
 * \param *wsClientDesc Pointer to the websocket client descriptor
 */
void websocketClient_close(void *wsSessionDescriptor)
{
  struct websocket_session_desc *wsSessionDesc = wsSessionDescriptor;
  if(wsSessionDesc == NULL)
    return;

  struct websocket_client_desc *wsDesc = wsSessionDesc->wsDesc.wsClientDesc;

  if(wsDesc == NULL)
    return;

  if(wsDesc->socketDesc != NULL)
  {
    socketClient_close(wsDesc->socketDesc);
    wsDesc->socketDesc = NULL;
  }
  wsDesc->session.state = WS_STATE_CLOSED;
  free(wsDesc->address);
  wsDesc->address = NULL;
  free(wsDesc->port);
  wsDesc->port = NULL;
  free(wsDesc->endpoint);
  wsDesc->endpoint = NULL;
  free(wsDesc->wsKey);
  wsDesc->wsKey = NULL;
  refcnt_unref(wsDesc);
}

/**
 * \brief returns if the client is still connected
 *
 * \return true => connected else false
 */
bool websocketClient_isConnected(void *wsSessionDescriptor)
{
  struct websocket_session_desc *wsSessionDesc = wsSessionDescriptor;

  return (wsSessionDesc->state != WS_STATE_CLOSED);
}

/**
 * \brief: closes the given websocket server
 *
 * \param *wsDesc: pointer to the websocket descriptor
 *
 */
void websocketServer_close(void *wsDesc)
{
  struct websocket_desc *websockDesc = wsDesc;

  socketServer_close(websockDesc->socketDesc);
  refcnt_unref(websockDesc);
}

/**
 * \brief: increments the reference count of the given object
 *
 * \param *ptr: poiner to the object
 */
void websocket_ref(void *ptr)
{
  refcnt_ref(ptr);
}

/**
 * \brief: decrements the reference count of the given object and frees it if necessary
 *
 * \param *ptr: poiner to the object
 */
void websocket_unref(void *ptr)
{
  refcnt_unref(ptr);
}


