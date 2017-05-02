/*
 * websocket.c
 *
 *  Created on: Mar 23, 2017
 *      Author: Clemens Kresser
 *      License: MIT
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "socket_server/socket_server.h"
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

#define MESSAGE_TIMEOUT_S 30

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

struct websocket_desc
{
  void (*ws_onMessage)(void *websocketUserData, void *clientDesc, void *clientUserData, enum ws_data_type dataType, void *msg, size_t len);
  void* (*ws_onOpen)(void *websocketUserData, void *clientDesc);
  void (*ws_onClose)(void *websocketUserData, void *clientDesc, void *clientUserData);
  void *socketDesc;
  void *wsSocketUserData;
};

struct last_message
{
  enum ws_data_type dataType;
  bool firstReceived;
  bool complete;
  unsigned long utf8Handle;
  size_t len;
  char *data;
};

struct websocket_client_desc
{
  void *socketClientDesc;
  enum ws_state state;
  struct last_message lastMessage;
  void *clientUserData;
  struct timespec timeout;
};

#define WS_ACCEPT_MAGIC_KEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/**
 * \brief: calculates the Sec-WebSocket-Accept from the Sec-WebSocket-Key
 *
 * \param *key: pointer to the key that should be used
 *
 * \return: to the string containing the Sec-WebSocket-Accept (must be freed after use)
 */
static char *calculateSecWebSocketAccept(char *key)
{
  char concatString[64];
  unsigned char sha1Hash[21];

  snprintf(concatString, sizeof(concatString), "%s" WS_ACCEPT_MAGIC_KEY, key);

  SHA1((char *) sha1Hash, concatString, strlen(concatString));

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

  log_dbg("%s() key:%s", __func__, key);

  return 0;
}

#define WS_HANDSHAKE_REPLY_BLUEPRINT "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"

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
  bool fin;
  enum ws_opcode opcode;
  size_t payloadLength;
  bool masked;
  unsigned char mask[4];
//  unsigned long mask;
  unsigned char payloadStartOffset;
};

/**
 * \brief: prints the websocket header (for debugging purpose only)
 *
 * \param *header: pointer to the header
 */
static void printWsHeader(struct ws_header *header)
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
static int parseWebsocketHeader(unsigned char *data, size_t len, struct ws_header *header)
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
    log_err("invalid header len to small(%zu)", len);
    return 0;
  }

  header->payloadLength = 0;

  //decode payload length
  if((data[1] & 0x7F) < 126)
  {
    header->payloadLength = data[1] & 0x7F;
    lengthNumBytes = 0; //not really true but needed for further calculations
  }
  else if((data[1] & 0x7F) == 126)
  {
    if(len < 4)
    {
      log_err("invalid header len to small(%zu)", len);
      return 0;
    }
    lengthNumBytes = 2;
  }
  else //data[1] == 127
  {
    if(len < 10)
    {
      log_err("invalid header len to small(%zu)", len);
      return 0;
    }
    lengthNumBytes = 8;
  }

  for(i = 0; i < lengthNumBytes; i++)
  {
    header->payloadLength <<= 8;
    header->payloadLength |= data[2 + i];
  }

  log_dbg("payloadlength:%zu", header->payloadLength);

  if(header->masked)
  {
    if(len < 2 + lengthNumBytes + 4)
    {
      log_err("invalid header len to small(%zu)", len);
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
 * \param *buffer: pointer to the buffer where the header should be written to
 *                 should be able to hold at least 10 bytes
 * \param opcode: the opcode that should be used for the header
 * \param fin: fin bit (is this the last frame (true) or will more follow (false))
 * \param len: the length of the payload
 *
 * \return: the length of the header
 */
static int createWebsocketHeader(unsigned char *buffer, enum ws_opcode opcode, bool fin, size_t len)
{
  int cnt, i;

  buffer[0] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);

  //masked bit always 0 for server->client replies
  if(len < 126)
  {
    buffer[1] = len;
    cnt = 2;
  }
  else if(len <= 0xFFFF)
  {
    buffer[1] = 126;
    buffer[2] = len >> 8;
    buffer[3] = len & 0xFF;
    cnt = 4;
  }
  else
  {
    buffer[1] = 127;
    cnt = 2;

    for(i = 7; i > -1; i--)
    {
      buffer[cnt] = (len >> (i * 8)) & 0xFF;
      cnt++;
    }
  }

  return cnt;
}

/**
 * \brief: sends data through websockets with custom opcodes
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param opcode: the opcode to use
 * \param fin: true if this is the last frame of a sequence else false
 * \param *msg: the payload data
 * \param len: the payload length
 *
 * \return 0 if successful else -1
 */
static int sendDataLowLevel(void *wsClientDesc, enum ws_opcode opcode, bool fin, void *msg, size_t len)
{
  struct websocket_client_desc *websockClientDesc = wsClientDesc;
  unsigned char header[10];
  int headerLength;
  unsigned char *sendBuffer;
  int rc;

  if(websockClientDesc->state == WS_STATE_CLOSED)
    return -1;

  headerLength = createWebsocketHeader(header, opcode, fin, len);

  sendBuffer = malloc(headerLength + len);
  if(!sendBuffer)
    return -1;
  memcpy(sendBuffer, header, headerLength);
  if(len)
    memcpy(&sendBuffer[headerLength], msg, len);

  rc = socketServer_send(websockClientDesc->socketClientDesc, sendBuffer, len + headerLength);
  free(sendBuffer);
  log_dbg("%s retv:%d", __func__, rc);
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
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param *data: pointer to the payload data
 * \param len: the length of the payload data
 * \param *header: pointer to the parsed websocket header structure
 *
 * \return: the message state
 */
static enum ws_msg_state handleFirstMessage(struct websocket_client_desc *wsClientDesc, unsigned char *data, size_t len, struct ws_header *header)
{
  size_t i;

  if(!header->masked)
  {
    websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(wsClientDesc->lastMessage.data != NULL)
  {
    log_err("last message not finished");
    websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(header->payloadLength) //it's allowed to send frames with payload length = 0
  {
    if(header->fin)
      wsClientDesc->lastMessage.data = refcnt_allocate(header->payloadLength, NULL);
    else
      wsClientDesc->lastMessage.data = malloc(header->payloadLength);
    if(!wsClientDesc->lastMessage.data)
    {
      log_err("refcnt_allocate failed dropping message");
      return WS_MSG_STATE_ERROR;
    }

    for(i = 0; i < header->payloadLength; i++)
    {
      wsClientDesc->lastMessage.data[i] = data[header->payloadStartOffset + i] ^ header->mask[i % 4];
    }
  }

  wsClientDesc->lastMessage.firstReceived = true;
  wsClientDesc->lastMessage.complete = header->fin;
  wsClientDesc->lastMessage.dataType = header->opcode == WS_OPCODE_TEXT ? WS_DATA_TYPE_TEXT : WS_DATA_TYPE_BINARY;

  wsClientDesc->lastMessage.len = header->payloadLength;
  if(wsClientDesc->lastMessage.dataType == WS_DATA_TYPE_TEXT)
  {
    enum utf8_state state;

    wsClientDesc->lastMessage.utf8Handle = 0;
    state = utf8_validate(wsClientDesc->lastMessage.data, wsClientDesc->lastMessage.len, &wsClientDesc->lastMessage.utf8Handle);
    if((header->fin && (state != UTF8_STATE_OK)) || (!header->fin && (state == UTF8_STATE_FAIL)))
    {
      log_err("no valid utf8 string closing connection");
      websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_INVALID_DATA);
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
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param *data: pointer to the payload data
 * \param len: the length of the payload data
 * \param *header: pointer to the parsed websocket header structure
 *
 * \return: the message state
 */
static enum ws_msg_state handleContMessage(struct websocket_client_desc *wsClientDesc, unsigned char *data, size_t len, struct ws_header *header)
{
  size_t i;
  char *temp;

  if((!wsClientDesc->lastMessage.firstReceived) || (!header->masked))
  {
    log_err("missing last message closing connection");
    websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }

  if(wsClientDesc->lastMessage.len + header->payloadLength) //it's allowed to send frames with payload length = 0
  {
    if(header->fin)
    {
      temp = refcnt_allocate(wsClientDesc->lastMessage.len + header->payloadLength, NULL);
      if(!temp)
      {
        log_err("refcnt_allocate failed dropping message");
        free(wsClientDesc->lastMessage.data);
        wsClientDesc->lastMessage.data = NULL;
        return WS_MSG_STATE_ERROR;
      }
      memcpy(temp, wsClientDesc->lastMessage.data, wsClientDesc->lastMessage.len);
      free(wsClientDesc->lastMessage.data);
      wsClientDesc->lastMessage.data = temp;
    }
    else
    {
      temp = realloc(wsClientDesc->lastMessage.data, wsClientDesc->lastMessage.len + header->payloadLength);
      if(!temp)
      {
        log_err("realloc failed dropping message");
        free(wsClientDesc->lastMessage.data);
        wsClientDesc->lastMessage.data = NULL;
        return WS_MSG_STATE_ERROR;
      }
      wsClientDesc->lastMessage.data = temp;
    }

    for(i = 0; i < header->payloadLength; i++)
    {
      wsClientDesc->lastMessage.data[wsClientDesc->lastMessage.len + i] = data[header->payloadStartOffset + i] ^ header->mask[i % 4];
    }
  }
  wsClientDesc->lastMessage.complete = header->fin;

  if(wsClientDesc->lastMessage.dataType == WS_DATA_TYPE_TEXT)
  {
    enum utf8_state state;
    state = utf8_validate(&wsClientDesc->lastMessage.data[wsClientDesc->lastMessage.len], header->payloadLength, &wsClientDesc->lastMessage.utf8Handle);
    if((header->fin && state != UTF8_STATE_OK) || (!header->fin && state == UTF8_STATE_FAIL))
    {
      log_err("no valid utf8 string closing connection");
      websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_INVALID_DATA);
      return WS_MSG_STATE_ERROR;
    }
  }
  wsClientDesc->lastMessage.len += header->payloadLength;

  if(header->fin)
    return WS_MSG_STATE_USER_DATA;
  else
    return WS_MSG_STATE_NO_USER_DATA;
}

/**
 * \brief: handles a ping message and replies with a pong message
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param *data: pointer to the payload data
 * \param len: the length of the payload data
 * \param *header: pointer to the parsed websocket header structure
 *
 * \return: the message state
 */
static enum ws_msg_state handlePingMessage(struct websocket_client_desc *wsClientDesc, unsigned char *data, size_t len, struct ws_header *header)
{
  int rc;
  char *temp;
  size_t i;

  if(header->fin)
  {
    if(header->payloadLength > 125)
    {
      websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
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

      if(sendDataLowLevel(wsClientDesc, WS_OPCODE_PONG, true, temp, header->payloadLength) == 0)
        rc = WS_MSG_STATE_NO_USER_DATA;
      else
        rc = WS_MSG_STATE_ERROR;
      free(temp);
      return rc;
    }
    else
    {
      if(sendDataLowLevel(wsClientDesc, WS_OPCODE_PONG, true, &data[header->payloadStartOffset], header->payloadLength) == 0)
        return WS_MSG_STATE_NO_USER_DATA;
      else
        return WS_MSG_STATE_ERROR;
    }
  }
  else
  {
    websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
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
static enum ws_msg_state handlePongMessage(struct websocket_client_desc *wsClientDesc, unsigned char *data, size_t len, struct ws_header *header)
{
  if(header->fin && (header->payloadLength <= 125))
  {
    //Pongs on serverside are ignored
    return WS_MSG_STATE_NO_USER_DATA;
  }
  else
  {
    websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
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
static enum ws_msg_state handleDisconnectMessage(struct websocket_client_desc *wsClientDesc, unsigned char *data, size_t len, struct ws_header *header)
{
  size_t i;
  int rc;
  unsigned long utf8Handle = 0;

  if((header->fin) && (header->payloadLength != 1) && (header->payloadLength <= 125))
  {
    enum ws_close_code code;

    if(!header->payloadLength)
    {
      websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_NORMAL);
      return WS_MSG_STATE_NO_USER_DATA;
    }
    else if(header->masked)
    {
      char tempBuffer[125];

      for(i = 0; i < header->payloadLength; i++)
      {
        tempBuffer[i] = data[header->payloadStartOffset + i] ^ header->mask[i % 4];
      }

      code = ((unsigned char) tempBuffer[0] << 8) | ((unsigned char) tempBuffer[1]);
      if(checkCloseCode(code) == false)
      {
        websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
        return WS_MSG_STATE_ERROR;
      }
      else if((header->payloadLength == 2) || (UTF8_STATE_OK == utf8_validate(&tempBuffer[2], header->payloadLength - 2, &utf8Handle)))
      {
        websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_NORMAL);
        return WS_MSG_STATE_NO_USER_DATA;
      }
      else
      {
        websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_INVALID_DATA);
        return WS_MSG_STATE_ERROR;
      }
    }
    else
    {
      if((header->payloadLength == 2) || utf8_validate((char*) &data[header->payloadStartOffset + 2], header->payloadLength - 2, &utf8Handle))
      {
        if(sendDataLowLevel(wsClientDesc, WS_OPCODE_DISCONNECT, true, &data[header->payloadStartOffset], header->payloadLength) == 0)
          rc = WS_MSG_STATE_NO_USER_DATA;
        else
          rc = WS_MSG_STATE_ERROR;
        socketServer_closeClient(wsClientDesc->socketClientDesc);
      }
      else
      {
        websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_INVALID_DATA);
        rc = WS_MSG_STATE_ERROR;
      }
      return rc;
    }
  }
  else
  {
    websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
    return WS_MSG_STATE_ERROR;
  }
}

/**
 * \brief: parses a message and stores it to the client descriptor
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param *data: pointer to the received data
 * \param len: the length of the data
 * \param header: the parsed header struct (as parsed by parseWebsocketHeader)
 *
 * \return one of ws_msg_state
 */
static enum ws_msg_state parseMessage(struct websocket_client_desc *wsClientDesc, unsigned char *data, size_t len, struct ws_header *header)
{
  enum ws_msg_state rc;

  log_dbg("->len:%zu", len);

  if(len < header->payloadStartOffset + header->payloadLength)
    return WS_MSG_STATE_INCOMPLETE;

  switch(header->opcode)
  {
    case WS_OPCODE_TEXT:
    case WS_OPCODE_BINARY:
      return handleFirstMessage(wsClientDesc, data, len, header);

    case WS_OPCODE_CONTINUATION:
      return handleContMessage(wsClientDesc, data, len, header);

    case WS_OPCODE_PING:
      return handlePingMessage(wsClientDesc, data, len, header);

    case WS_OPCODE_PONG:
      return handlePongMessage(wsClientDesc, data, len, header);

    case WS_OPCODE_DISCONNECT:
      return handleDisconnectMessage(wsClientDesc, data, len, header);

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
 * \return: pointer to the websocket client descriptor
 */
static void *websocket_onOpen(void *socketUserData, void *socketClientDesc)
{
  struct websocket_desc *wsDesc = socketUserData;
  struct websocket_client_desc *wsClientDesc;

  refcnt_ref(socketClientDesc);
  wsClientDesc = refcnt_allocate(sizeof(struct websocket_client_desc), NULL);
  wsClientDesc->socketClientDesc = socketClientDesc;
  wsClientDesc->state = WS_STATE_HANDSHAKE;
  wsClientDesc->timeout.tv_nsec = 0;
  wsClientDesc->timeout.tv_sec = 0;
  wsClientDesc->lastMessage.data = NULL;
  wsClientDesc->lastMessage.len = 0;
  wsClientDesc->lastMessage.complete = false;

  if(wsDesc->ws_onOpen)
    wsClientDesc->clientUserData = wsDesc->ws_onOpen(wsDesc, wsClientDesc);

  return wsClientDesc;
}

/**
 * \brief: function that gets called when a connection to a client is closed
 *         frees the websocket client descriptor
 *
 * \param *socketUserData: in this case this is the websocket descriptor
 * \param *socketClientDesc: the client descriptor from the socket server
 * \param *clientUserData: in this case this is the websocket client descriptor
 *
 */
static void websocket_onClose(void *socketUserData, void *socketClientDesc, void *clientUserData)
{
  struct websocket_desc *wsDesc = socketUserData;
  struct websocket_client_desc *wsClientDesc = clientUserData;

  if(wsClientDesc->lastMessage.data && wsClientDesc->lastMessage.complete)
    refcnt_unref(wsClientDesc->lastMessage.data);
  else
    free(wsClientDesc->lastMessage.data);
  wsClientDesc->lastMessage.data = NULL;

  if(wsClientDesc->state != WS_STATE_CLOSED)
  {
    wsClientDesc->state = WS_STATE_CLOSED;

    if(wsDesc->ws_onClose)
      wsDesc->ws_onClose(wsDesc->wsSocketUserData, wsClientDesc, wsClientDesc->clientUserData);
  }
  if(wsClientDesc->socketClientDesc)
    refcnt_unref(wsClientDesc->socketClientDesc);
  wsClientDesc->socketClientDesc = NULL;
  refcnt_unref(wsClientDesc);
}

/**
 * \brief: function that gets called when a message arrives at the socket server
 *
 * \param *socketUserData: in this case this is the websocket descriptor
 * \param *socketClientDesc: the client descriptor from the socket server
 * \param *clientUserData: in this case this is the websocket client descriptor
 * \param *msg: pointer to the buffer containing the data
 * \param len: the length of msg
 *
 * \return: the amount of bytes read
 *
 */
static size_t websocket_onMessage(void *socketUserData, void *socketClientDesc, void *clientUserData, void *msg, size_t len)
{
  struct websocket_desc *wsDesc = socketUserData;
  struct websocket_client_desc *wsClientDesc = clientUserData;
  struct ws_header wsHeader;
  struct timespec now;

  char key[WS_HS_KEY_LEN];
  char *replyKey;

  switch(wsClientDesc->state)
  {
    case WS_STATE_HANDSHAKE:
      if(parseHttpHeader(msg, len, key) == 0)
      {
        replyKey = calculateSecWebSocketAccept(key);

        if(debugEnabled)
          log_dbg("%s() replyKey:%s", __func__, replyKey);

        sendWsHandshakeReply(socketClientDesc, replyKey);

        free(replyKey);
        wsClientDesc->state = WS_STATE_CONNECTED;
      }
      else
      {
        log_err("parseHttpHeader failed");
      }
      return len;

    case WS_STATE_CONNECTED:
      switch(parseWebsocketHeader(msg, len, &wsHeader))
      {
        case -1:
          log_err("couldn't parse header");
          websocket_closeClient(wsClientDesc, WS_CLOSE_CODE_PROTOCOL_ERROR);
          return len;

        case 0:
          return 0;
          break;

        case 1:
          break;
      }
      if(debugEnabled)
        printWsHeader(&wsHeader);

      switch(parseMessage(wsClientDesc, msg, len, &wsHeader))
      {
        case WS_MSG_STATE_NO_USER_DATA:
          wsClientDesc->timeout.tv_nsec = 0;
          wsClientDesc->timeout.tv_sec = 0;
          return wsHeader.payloadLength + wsHeader.payloadStartOffset;

        case WS_MSG_STATE_USER_DATA:
          if(wsDesc->ws_onMessage)
            wsDesc->ws_onMessage(wsDesc->wsSocketUserData, wsClientDesc, wsClientDesc->clientUserData, wsClientDesc->lastMessage.dataType,
                wsClientDesc->lastMessage.data, wsClientDesc->lastMessage.len);
          if(wsClientDesc->lastMessage.data)
            refcnt_unref(wsClientDesc->lastMessage.data);
          wsClientDesc->lastMessage.data = NULL;
          wsClientDesc->lastMessage.complete = false;
          wsClientDesc->lastMessage.firstReceived = false;
          wsClientDesc->lastMessage.len = 0;
          wsClientDesc->timeout.tv_nsec = 0;
          wsClientDesc->timeout.tv_sec = 0;
          return wsHeader.payloadLength + wsHeader.payloadStartOffset;

        case WS_MSG_STATE_INCOMPLETE:
          clock_gettime(CLOCK_MONOTONIC, &now);
          if(wsClientDesc->timeout.tv_sec == (wsClientDesc->timeout.tv_nsec == 0))
          {
            wsClientDesc->timeout = now;
          }
          else if(wsClientDesc->timeout.tv_sec > now.tv_sec + MESSAGE_TIMEOUT_S)
          {
            free(wsClientDesc->lastMessage.data);
            wsClientDesc->lastMessage.data = NULL;
            wsClientDesc->lastMessage.len = 0;
            wsClientDesc->lastMessage.complete = 0;
            wsClientDesc->timeout.tv_sec = 0;
            wsClientDesc->timeout.tv_nsec = 0;
            log_err("message timeout");
            return len;
          }
          return 0;

        case WS_MSG_STATE_ERROR:
          if(wsClientDesc->lastMessage.data && wsClientDesc->lastMessage.complete)
            refcnt_unref(wsClientDesc->lastMessage.data);
          else
            free(wsClientDesc->lastMessage.data);
          wsClientDesc->lastMessage.data = NULL;
          wsClientDesc->lastMessage.len = 0;
          wsClientDesc->lastMessage.complete = 0;
          wsClientDesc->timeout.tv_sec = 0;
          wsClientDesc->timeout.tv_nsec = 0;
          return len;

        default:
          log_err("unexpected return value");
          return len;
      }
      break;

    case WS_STATE_CLOSED:
      log_err("websocket closed ignoring message");
      return len;
  }

  return 0;
}

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
int websocket_sendData(void *wsClientDesc, enum ws_data_type dataType, void *msg, size_t len)
{
  unsigned char opcode;
  struct websocket_client_desc *websockClientDesc = wsClientDesc;

  if (websockClientDesc->state != WS_STATE_CONNECTED)
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

  return sendDataLowLevel(wsClientDesc, opcode, true, msg, len);
}

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
int websocket_sendDataFragmentedStart(void *wsClientDesc, enum ws_data_type dataType, void *msg, size_t len)
{
  unsigned char opcode;
  struct websocket_client_desc *websockClientDesc = wsClientDesc;

  if (websockClientDesc->state != WS_STATE_CONNECTED)
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

  return sendDataLowLevel(wsClientDesc, opcode, false, msg, len);
}

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
int websocket_sendDataFragmentedCont(void *wsClientDesc, bool fin, void *msg, size_t len)
{
  struct websocket_client_desc *websockClientDesc = wsClientDesc;

  if (websockClientDesc->state != WS_STATE_CONNECTED)
    return -1;

  return sendDataLowLevel(wsClientDesc, WS_OPCODE_CONTINUATION, fin, msg, len);
}

/**
 * \brief: closes the given websocket client
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 * \param code: the closing code
 */
void websocket_closeClient(void *wsClientDesc, enum ws_close_code code)
{
  struct websocket_client_desc *websockClientDesc = wsClientDesc;
  unsigned char help[2];
//  log_dbg("%s()",__func__);

  help[0] = code >> 8;
  help[1] = code & 0xFF;

  sendDataLowLevel(wsClientDesc, WS_OPCODE_DISCONNECT, true, help, 2);

  if(websockClientDesc->lastMessage.data && websockClientDesc->lastMessage.complete)
    refcnt_unref(websockClientDesc->lastMessage.data);
  else
    free(websockClientDesc->lastMessage.data);

  websockClientDesc->lastMessage.data = NULL;
  websockClientDesc->lastMessage.len = 0;
  websockClientDesc->lastMessage.complete = 0;

  socketServer_closeClient(websockClientDesc->socketClientDesc);
}

/**
 * \brief: returns the user data of the given client
 *
 * \param *wsClientDesc: pointer to the websocket client descriptor
 *
 */
void *websocket_getClientUserData(void *wsClientDesc)
{
  struct websocket_client_desc *wsClientDescriptor = wsClientDesc;

  return wsClientDescriptor->clientUserData;
}

/**
 * \brief: opens a websocket server
 *
 * \param wsInit: pointer to the init struct
 * \param websocketUserData: userData for the socket
 *
 * \return: the websocket descriptor or NULL in case of error
 */
void *websocket_open(struct websocket_init *wsInit, void *websocketUserData)
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
 * \brief: closes the given websocket
 *
 * \param *wsDesc: pointer to the websocket descriptor
 *
 */
void websocket_close(void *wsDesc)
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


