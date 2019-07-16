/*
 * Amazon FreeRTOS HTTPS Client V1.0.0
 * Copyright (C) 2019 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/**
 * @file iot_https_client.h
 * @brief Implementation of the user-facing functions of the Amazon FreeRTOS HTTPS Client library. 
 */

/* The config header is always included first. */
#include "iot_config.h"

/* HTTPS Client library private inclues. */
#include "private/iot_https_internal.h"

/*-----------------------------------------------------------*/

/**
 * @brief Partial HTTPS request first line.
 * 
 * This is used for the calculation of the requestUserBufferMinimumSize. 
 * The minimum path is "/" because we cannot know how long the application requested path is is going to be. 
 * CONNECT is the longest string length HTTP method according to RFC 2616.
 */
#define HTTPS_PARTIAL_REQUEST_LINE              HTTPS_CONNECT_METHOD " " HTTPS_EMPTY_PATH " " HTTPS_PROTOCOL_VERSION

/**
 * @brief The User-Agent header line string.
 * 
 * This is of the form:
 * "User-Agent: <configured-user-agent>\r\n"
 * This is used for the calculation of the requestUserBufferMinimumSize.
 */
#define HTTPS_USER_AGENT_HEADER_LINE            HTTPS_USER_AGENT_HEADER HTTPS_HEADER_FIELD_SEPARATOR IOT_HTTPS_USER_AGENT HTTPS_END_OF_HEADER_LINES_INDICATOR

/**
 * @brief The Host header line with the field only and not the value.
 * 
 * This is of the form:
 * "Host: \r\n"
 * This is used for the calculation of the requestUserBufferMinimumSize. The Host value is not specified because we
 * cannot anticipate what server the client is making requests to.
 */
#define HTTPS_PARTIAL_HOST_HEADER_LINE          HTTPS_HOST_HEADER HTTPS_HEADER_FIELD_SEPARATOR HTTPS_END_OF_HEADER_LINES_INDICATOR   

/**
 * @brief The maximum Content-Length header line size.
 * 
 * This is the length of header line string: "Content-Length: 4294967296\r\n". 4294967296 is 2^32. This number is chosen
 * because it is the maximum file size that can be represented in a 32 bit system.
 * 
 * This is used to initialize a local array for the final headers to send.
 */
#define HTTPS_MAX_CONTENT_LENGTH_LINE_LENGTH    ( 26 )

/*
 * String constants for the Connection header and possible values.
 * 
 * This is used for writing headers automatically during the sending of the HTTP request.
 * "Connection: keep-alive\r\n" is written automatically for a persistent connection.
 * "Connection: close\r\n" is written automatically for a non-presistent connection.
 */
#define HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE HTTPS_CONNECTION_HEADER HTTPS_HEADER_FIELD_SEPARATOR HTTPS_CONNECTION_KEEP_ALIVE_HEADER_VALUE HTTPS_END_OF_HEADER_LINES_INDICATOR
#define HTTPS_CONNECTION_CLOSE_HEADER_LINE      HTTPS_CONNECTION_HEADER HTTPS_HEADER_FIELD_SEPARATOR HTTPS_CONNECTION_CLOSE_HEADER_VALUE HTTPS_END_OF_HEADER_LINES_INDICATOR

/**
 * @brief The length of the "Connection: keep-alive\r\n" header. 
 * 
 * This is used for sizing a local buffer for the final headers to send that include the "Connection: keep-alive\r\n" 
 * header line. 
 * 
 * This is used to initialize a local array for the final headers to send.
 */
#define HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE_LENGTH      ( 24 )

/*-----------------------------------------------------------*/

/**
 * @brief Minimum size of the request user buffer.
 * 
 * The request user buffer is configured in IotHttpsClientRequestInfo_t.reqUserBuffer. This buffer stores the internal context
 * of the request and then the request headers right after. The minimum size for the buffer is the total size of the 
 * internal request context, the HTTP formatted request line, the User-Agent header line, and the part of the Host 
 * header line.
 */
const uint32_t requestUserBufferMinimumSize = sizeof(_httpsRequest_t) + 
                sizeof(HTTPS_PARTIAL_REQUEST_LINE) +
                sizeof(HTTPS_USER_AGENT_HEADER_LINE) +
                sizeof(HTTPS_PARTIAL_HOST_HEADER_LINE);

/**
 * @brief Minimum size of the response user buffer.
 * 
 * The response user buffer is configured in IotHttpsClientRequestInfo_t.respUserBuffer. This buffer stores the internal context
 * of the response and then the response headers right after. This minimum size is calculated for the case if no bytes
 * from the HTTP response headers are to be stored. 
 */
const uint32_t responseUserBufferMinimumSize = sizeof(_httpsResponse_t);

/**
 * @brief Minimum size of the connection user buffer.
 * 
 * The connection user buffer is configured in IotHttpsConnectionInfo_t.userBuffer. This buffer stores the internal context of the 
 * connection.
 */
const uint32_t connectionUserBufferMinimumSize = sizeof(_httpsConnection_t);

/*-----------------------------------------------------------*/

/**
 * @brief Callback from http-parser to indicate the start of the HTTP response message is reached.
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_message_begin.
 */
static int _httpParserOnMessageBeginCallback(http_parser * pHttpParser);

/**
 * @brief Callback from http-parser to indicate it found the HTTP response status code.
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * @param[in] pLoc - Pointer to the HTTP response status code string in the response message buffer.
 * @param[in] length - The length of the HTTP response status code string.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_status.
 */
static int _httpParserOnStatusCallback(http_parser * pHttpParser, const char * pLoc, size_t length);

/**
 * @brief Callback from http-parser to indicate it found an HTTP response header field.
 * 
 * If only part of the header field was returned here in this callback, then this callback will be invoked again the
 * next time the parser executes on the next part of the header field.
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * @param[in] pLoc - Pointer to the header field string in the response message buffer.
 * @param[in] length - The length of the header field.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_header_field.
 */
static int _httpParserOnHeaderFieldCallback(http_parser * pHttpParser, const char * pLoc, size_t length);

/**
 * @brief Callback from http-parser to indicate it found an HTTP response header value.
 * 
 * This value corresponds to the field that was found in the _httpParserOnHeaderFieldCallback() called immediately 
 * before this callback was called.
 * 
 * If only part of the header value was returned here in this callback, then this callback will be invoked again the
 * next time the parser executes on the next part of the header value.
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * @param[in] pLoc - Pointer to the header value string in the response message buffer.
 * @param[in] length - The length of the header value.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_header_value.
 */
static int _httpParserOnHeaderValueCallback(http_parser * pHttpParser, const char * pLoc, size_t length);

/**
 * @brief Callback from http-parser to indicate it reached the end of the headers in the HTTP response messsage.
 * 
 * The end of the headers is signalled in a HTTP response message by another "\r\n" after the final header line.
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_headers_complete.
 */
static int _httpParserOnHeadersCompleteCallback(http_parser * pHttpParser);

/**
 * @brief Callback from http-parser to indicate it found HTTP response body.
 * 
 * This callback will be invoked multiple times if the response body is of "Transfer-Encoding: chunked". 
 * _httpParserOnChunkHeaderCallback() will be invoked first, then _httpParserOnBodyCallback(), then 
 * _httpParserOnChunkCompleteCallback(), then repeated back to _httpParserOnChunkHeaderCallback() if there are more
 * "chunks".
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * @param[in] pLoc - Pointer to the body string in the response message buffer.
 * @param[in] length - The length of the body found.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_body.
 */
static int _httpParserOnBodyCallback(http_parser * pHttpParser, const char * pLoc, size_t length);

/**
 * @brief Callback from http-parser to indicate it reached the end of the HTTP response messsage.
 * 
 * The end of the message is signalled in a HTTP response message by another "\r\n" after the final header line, with no
 * entity body; or it is singalled by "\r\n" at the end of the entity body.
 * 
 * For a Transfer-Encoding: chunked type of response message, the end of the message is signalled by a terminating 
 * chunk header with length zero.
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_message_complete.
 */
static int _httpParserOnMessageCompleteCallback(http_parser * pHttpParser);

/* This code prints debugging information and is, therefore, compiled only when
 * log level is set to IOT_LOG_DEBUG. */
#if (LIBRARY_LOG_LEVEL == IOT_LOG_DEBUG)
/**
 * @brief Callback from http-parser to indicate it found an HTTP Transfer-Encoding: chunked header.
 * 
 * Transfer-Encoding: chunked headers are embedded in the HTTP response entity body by a "\r\n" followed by the size of
 * the chunk followed by another "\r\n".
 * 
 * If only part of the header field was returned here in this callback, then this callback will be invoked again the
 * next time the parser executes on the next part of the header field.
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_chunk_header.
 */
static int _httpParserOnChunkHeaderCallback(http_parser * pHttpParser);

/**
 * @brief Callback from http-parser to indicate it reached the end of an HTTP response messsage "chunk".
 * 
 * A chunk is complete when the chunk header size is read fully in the body.
 * 
 * See https://github.com/nodejs/http-parser for more information.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_chunk_complete.
 */
static int _httpParserOnChunkCompleteCallback(http_parser * pHttpParser);
#endif

/**
 * @brief Network receive callback for the HTTPS Client library.
 * 
 * This function is called by the network layer whenever data is available for the HTTP library.
 * 
 * @param[in] pNetworkConnection - The network connection with the HTTPS connection, passed by the network stack.
 * @param[in] pReceiveContext - A pointer to the HTTPS Client connection handle for which the packet was received.
 */
static void _networkReceiveCallback( void* pNetworkConnection, void* pReceiveContext );

/**
 * @brief Connects to HTTPS server and initializes the connection context.
 * 
 * @param[out] pConnHandle - The out parameter to return handle representing the open connection.
 * @param[in] pConnInfo - The connection configuration.
 * 
 * @return #IOT_HTTPS_OK if the connection context initialization was successful.
 *         #IOT_HTTPS_CONNECTION_ERROR if the connection failed.
 *         #IOT_HTTPS_INTERNAL_ERROR if the context initialization failed.
 */
static IotHttpsReturnCode_t _createHttpsConnection(IotHttpsConnectionHandle_t * pConnHandle, IotHttpsConnectionInfo_t *pConnInfo);

/**
 * @brief Disconnects from the network.
 * 
 * @param[in] pHttpsConnection - HTTPS connection handle.
 */
static void _networkDisconnect(_httpsConnection_t* pHttpsConnection);

/**
 * @brief Destroys the network connection.
 * 
 * @param[in] pHttpsConnection - HTTPS connection handle.
 */
static void _networkDestroy(_httpsConnection_t* pHttpsConnection);

/**
 * @brief Add a header to the current HTTP request.
 * 
 * The headers are stored in reqHandle->pHeaders.
 * 
 * @param[in] pHttpsRequest - HTTP request context.
 * @param[in] pName - The name of the header to add. This is a NULL terminated string.
 * @param[in] pValue - The buffer containing the value string.
 * @param[in] valueLen - The length of the header value string.
 * 
 * @return #IOT_HTTPS_OK if the header was added to the request successfully.
 *         #IOT_HTTPS_INSUFFICIENT_MEMORY if there was not enough room in the IotHttpsRequestHandle_t->pHeaders.
 */
static IotHttpsReturnCode_t _addHeader(_httpsRequest_t * pHttpsRequest, const char * pName, const char * pValue, uint32_t valueLen );

/**
 * @brief Send data on the network.
 * 
 * @param[in] pHttpsConnection - HTTP connection context.
 * @param[in] pBuf - The buffer containing the data to send.
 * @param[in] len - The length of the data to send.
 * 
 * @return #IOT_HTTPS_OK if the data sent successfully.
 *         #IOT_HTTPS_NETWORK_ERROR if there was an error sending the data on the network.
 */
static IotHttpsReturnCode_t _networkSend(_httpsConnection_t* pHttpsConnection, uint8_t * pBuf, size_t len);

/**
 * @brief Receive data on the network.
 * 
 * @param[in] pHttpsConnection - HTTP connection context.
 * @param[in] pBuf - The buffer to receive the data into.
 * @param[in] len - The length of the data to receive.
 * 
 * @return #IOT_HTTPS_OK if the data was received successfully.
 *         #IOT_HTTPS_NETWORK_ERROR if there was an error receiving the data on the network.
 *         #IOT_HTTPS_TIMEOUT_ERROR if we timedout trying to receive data from the network.
 */
static IotHttpsReturnCode_t _networkRecv( _httpsConnection_t* pHttpsConnection, uint8_t * pBuf, size_t bufLen);

/**
 * @brief Send all of the HTTP request headers in the pHeadersBuf and the final Content-Length and Connection headers.
 * 
 * All of the headers in headerbuf are sent first followed by the computed content length and persistent connection 
 * indication.
 * 
 * @param[in] pHttpsConnection - HTTP connection context.
 * @param[in] pHeadersBuf - The buffer containing the request headers to send. This buffer must contain HTTP headers
 *            lines without the indicator for the the end of the HTTP headers.
 * @param[in] headersLength - The length of the request headers to send.
 * @param[in] isNonPersistent - Indicator of whether the connection is persistent or not.
 * @param[in] contentLength - The length of the request body used for automatically creating a "Content-Length" header.
 * 
 * @return #IOT_HTTPS_OK if the headers were fully sent successfully.
 *         #IOT_HTTPS_NETWORK_ERROR if there was an error receiving the data on the network.
 *         #IOT_HTTPS_TIMEOUT_ERROR if we timedout trying to receive data from the network.
 */
static IotHttpsReturnCode_t _sendHttpsHeaders( _httpsConnection_t* pHttpsConnection, uint8_t* pHeadersBuf, uint32_t headersLength, bool isNonPersistent, uint32_t contentLength);

/**
 * @brief Send all of the HTTP request body in pBodyBuf.
 * 
 * @param[in] pHttpsConnection - HTTP connection context.
 * @param[in] pBodyBuf - Buffer of the request body to send.
 * @param[in] bodyLength - The length of the body to send.
 * 
 * @return #IOT_HTTPS_OK if the body was fully sent successfully.
 *         #IOT_HTTPS_NETWORK_ERROR if there was an error receiving the data on the network.
 *         #IOT_HTTPS_TIMEOUT_ERROR if we timedout trying to receive data from the network.
 */
static IotHttpsReturnCode_t _sendHttpsBody( _httpsConnection_t* pHttpsConnection, uint8_t* pBodyBuf, uint32_t bodyLength);

/**
 * @brief Parse the HTTP response message in pBuf.
 * 
 * @param[in] pHttpParserInfo - Pointer to the information containing the instance of the http-parser and the execution function.
 * @param[in] pBuf - The buffer containing the data to parse.
 * @param[in] len - The length of data to parse.
 * 
 * @return #IOT_HTTPS_OK if the data was parsed successfully.
 *         #IOT_HTTPS_PARSING_ERROR if there was an error with parsing the data.
 */
static IotHttpsReturnCode_t _parseHttpsMessage(_httpParserInfo_t* pHttpParserInfo, char* pBuf, size_t len);

/**
 * @brief Receive any part of an HTTP response.
 * 
 * This function is used for both receiving the body into the body buffer and receiving the header into the header 
 * buffer.
 * 
 * @param[in] pHttpsConnection - HTTP Connection context.
 * @param[in] pParser - Pointer to the instance of the http-parser.
 * @param[in] pCurrentParserState - The current state of what has been parsed in the HTTP response.
 * @param[in] finalParserState - The final state of the parser expected after this function finishes.
 * @param[in] pBufCur - Pointer to the next location to write data into the buffer pBuf. This is double pointer to update the response context buffer pointers.
 * @param[in] pBufEnd - Pointer to the end of the buffer to receive the HTTP response into.
 * @param[out] pNetworkStatus - The network status will be returned here. The network status can be any of the return values from _networkRecv().
 * 
 * @return #IOT_HTTPS_OK if we received the HTTP response message part successfully.
 *         #IOT_HTTPS_PARSING_ERROR if there was an error with parsing the data.
 */
static IotHttpsReturnCode_t _receiveHttpsMessage( _httpsConnection_t* pHttpsConnection, 
                                                  _httpParserInfo_t* pParser,
                                                  IotHttpsResponseParserState_t *pCurrentParserState,
                                                  IotHttpsResponseParserState_t finalParserState, 
                                                  uint8_t** pBufCur,
                                                  uint8_t** pBufEnd,
                                                  IotHttpsReturnCode_t *pNetworkStatus);

/**
 * @brief Receive the HTTP response headers.
 * 
 * If the Content-Length header field is found in the received headers, then #IotHttpsResponseInternal_t.contentLength 
 * will be set and available. 
 *  
 * Receiving the response headers is always the first step in receiving the response, therefore the 
 * pHttpsResponse->httpParserInfo will be initialized to a starting state when this function is called.
 * 
 * This function also sets internal states to indicate that the header buffer is being processed now for a new response.
 * 
 * @param[in] pHttpsConnection - HTTP connection context.
 * @param[in] pHttpsResponse - HTTP response context.
 * @param[out] pNetworkStatus - The network status will be returned here. The network status can be any of the return values from _networkRecv().
 * 
 * @return #IOT_HTTPS_OK if we received the HTTP headers successfully.
 *         #IOT_HTTPS_PARSING_ERROR if there was an error with parsing the header buffer.
 */
static IotHttpsReturnCode_t _receiveHttpsHeaders( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t *pNetworkStatus);

/**
 * @brief Receive the HTTP response body.
 * 
 * Sets internal states to indicate that the the body buffer is being processed now for a new response.
 * 
 * @param[in] pHttpsConnection - HTTP connection context.
 * @param[in] pHttpsResponse - HTTP response context.
 * @param[out] pNetworkStatus - The network status will be returned here. The network status can be any of the return values from _networkRecv().
 * 
 * @return #IOT_HTTPS_OK if we received the HTTP body successfully.
 *         #IOT_HTTPS_PARSING_ERROR if there was an error with parsing the body buffer.
 */
static IotHttpsReturnCode_t _receiveHttpsBody( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t *pNetworkStatus);

/**
 * @brief Read the rest of any HTTP response that may be on the network.
 * 
 * This reads the rest of any left over response data that might still be on the network buffers. We do not want this
 * data left over because it will spill into the header and body buffers of next response that we try to receive. 
 * 
 * If we performed a request without a body and the headers received exceeds the size of the 
 * _httpResponse->pHeaders buffer, then we need to flush the network buffer.
 * 
 * If the application configured the body buffer as null in IotHttpsResponseInfo_t.syncInfo.respData and the server 
 * sends body in the response, but it exceeds the size of  _httpResponse->pHeaders buffer, then we need to flush the 
 * network buffer.
 * 
 * If the amount of body received on the network does not fit into a non-null IotHttpsResponseInfo_t.syncInfo.respData, 
 * then we need to flush the network buffer.
 * 
 * If an asynchronous request cancels in the middle of a response process, after already sending the request message, 
 * then we need to flush the network buffer.
 * 
 * @param[in] pHttpsConnection - HTTP connection context.
 * @param[in] pHttpsResponse - HTTP response context.
 * 
 * @return #IOT_HTTPS_OK if we successfully flushed the network data.
 *         #IOT_HTTPS_PARSING_ERROR if there was an error with parsing the data.
 *         #IOT_HTTPS_NETWORK_ERROR if there was an error receiving the data on the network.
 *         #IOT_HTTPS_TIMEOUT_ERROR if we timedout trying to receive data from the network.
 */
static IotHttpsReturnCode_t _flushHttpsNetworkData( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse );

/*-----------------------------------------------------------*/

/**
 * @brief Definition of the http-parser settings.
 * 
 * The http_parser_settings holds all of the callbacks invoked by the http-parser.
 */
static http_parser_settings _httpParserSettings = { 0 };

/*-----------------------------------------------------------*/

static int _httpParserOnMessageBeginCallback(http_parser * pHttpParser)
{
    IotLogDebug("Parser: Start of HTTPS Response message.");
    
    _httpsResponse_t *pHttpsResponse = (_httpsResponse_t*)(pHttpParser->data);
    /* Set the state of the parser. The headers are at the start of the message always. */
    pHttpsResponse->parserState = PARSER_STATE_IN_HEADERS;
    return 0;
}

/*-----------------------------------------------------------*/

static int _httpParserOnStatusCallback(http_parser * pHttpParser, const char * pLoc, size_t length)
{
    _httpsResponse_t *pHttpsResponse = (_httpsResponse_t*)(pHttpParser->data);
    IotLogDebug("Parser: Status %.*s retrieved from HTTPS response.", length, pLoc);

    /* Save the status code so it can be retrieved with IotHttpsClient_ReadResponseStatus(). */
    pHttpsResponse->status = (uint16_t)(pHttpParser->status_code);
    /* If we are parsing the network data received in the header buffer then we
       increment pHttpsResponse->pHeadersCur. The status line in the response is
       part of the data stored in header buffer _httpResponse->pHeaders. */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_FILLING_HEADER_BUFFER)
    {
        /* pHeadersCur will never exceed the pHeadersEnd here because PROCESSING_STATE_FILLING_HEADER_BUFFER 
           indicates we are currently in the header buffer and the total size of the header buffer is passed
           into http_parser_execute() as the maximum length to parse. */
        pHttpsResponse->pHeadersCur = (uint8_t*)(pLoc += length);
    }
    return 0;
}

/*-----------------------------------------------------------*/

static int _httpParserOnHeaderFieldCallback(http_parser * pHttpParser, const char * pLoc, size_t length)
{
    IotLogDebug("Parser: HTTPS header field parsed %.*s", length, pLoc);

    _httpsResponse_t * pHttpsResponse = (_httpsResponse_t *)(pHttpParser->data);
    /* If we are parsing the network data received in the header buffer then we can increment 
      pHttpsResponse->pHeadersCur. */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_FILLING_HEADER_BUFFER)
    {
        pHttpsResponse->pHeadersCur = (uint8_t*)(pLoc += length);
    }
    /* If the IotHttpsClient_ReadHeader() was called, then we check for the header field of interest. */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_SEARCHING_HEADER_BUFFER)
    {
        if(strncmp(pHttpsResponse->pReadHeaderField, pLoc, length) == 0)
        {
            pHttpsResponse->foundHeaderField = true;   
        }
    }
    return 0;
}

/*-----------------------------------------------------------*/

static int _httpParserOnHeaderValueCallback(http_parser * pHttpParser, const char * pLoc, size_t length)
{
    int retVal = 0;

    IotLogDebug("Parser: HTTPS header value parsed %.*s", length, pLoc);
    _httpsResponse_t * pHttpsResponse = (_httpsResponse_t *)(pHttpParser->data);
    /* If we are parsing the network data received in the header buffer then we can increment 
      pHttpsResponse->pHeadersCur. */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_FILLING_HEADER_BUFFER)
    {
        pHttpsResponse->pHeadersCur = (uint8_t*)(pLoc += length);
    }

    /* If the IotHttpsClient_ReadHeader() was called, then we check if we found the header field of interest. */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_SEARCHING_HEADER_BUFFER)
    {
        if(pHttpsResponse->foundHeaderField)
        {
            pHttpsResponse->pReadHeaderValue = ( char* )( pLoc );
            pHttpsResponse->readHeaderValueLength = length;
            /* We found a header field so we don't want to keep parsing.*/
            retVal = 1;
        }
    }
    return retVal;
}

/*-----------------------------------------------------------*/

static int _httpParserOnHeadersCompleteCallback(http_parser * pHttpParser)
{
    IotLogDebug("Parser: End of the headers reached in the pHttpParser.");

    int retVal = 0;
    _httpsResponse_t * pHttpsResponse = (_httpsResponse_t *)(pHttpParser->data);
    pHttpsResponse->parserState = PARSER_STATE_HEADERS_COMPLETE;

    /* If the IotHttpsClient_ReadHeader() was called, we return after finishing looking through all of the headers. 
       Returning a non-zero value exits the http parsing. */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_SEARCHING_HEADER_BUFFER)
    {
        retVal = 1;
    }
    /* When in this callback the pheaderCur pointer is at the first "\r" in the last header line. HTTP/1.1
    headers end with another "\r\n" at the end of the last line. This means we must increment
    the headerCur pointer to the length of "\r\n\r\n". */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_FILLING_HEADER_BUFFER)
    {
        pHttpsResponse->pHeadersCur += ( 2 * HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH );
    }

    /* content_length will be zero if no Content-Length header found by the parser. */
    pHttpsResponse->contentLength = (uint32_t)(pHttpParser->content_length);
    IotLogDebug("Parser: Content-Length found is %d.", pHttpsResponse->contentLength);

    if(pHttpsResponse->bufferProcessingState < PROCESSING_STATE_FINISHED)
    {
        /* For a HEAD method, there is no body expected in the response, so we return 1 to skip body parsing. 
        Also if it was configured in a synchronous response to ignore the HTTPS response body then also stop the body 
        parsing. */
        if((pHttpsResponse->method == IOT_HTTPS_METHOD_HEAD) || (pHttpsResponse->pBody == NULL))
        {
            retVal = 1;
        }
    }

    return retVal;
}

/*-----------------------------------------------------------*/

static int _httpParserOnBodyCallback(http_parser * pHttpParser, const char * pLoc, size_t length)
{
    IotLogDebug("Parser: Reached the HTTPS message body. It is of length: %d", length);

    _httpsResponse_t * pHttpsResponse = (_httpsResponse_t *)(pHttpParser->data);
    pHttpsResponse->parserState = PARSER_STATE_IN_BODY;

    /* Has the user provided a buffer and is it large enough to fit the body? The
    case of body buffer not being large enough can happen if the body was received
    in the header buffer and the body buffer can not fit in all the body. */
    if((pHttpsResponse->pBodyCur != NULL) && (pHttpsResponse->pBodyEnd - pHttpsResponse->pBodyCur > 0))
    {
        /* There are two scenarios when we need to copy data around:
        1. Some or all of the response body may have been received in the header
        buffer. If that is the case, we copy the response body received in the
        header buffer to the user provided body buffer.
        2. When we receive chunked header, the actual body is separated in
        multiple chunks which are preceeded by length. For example, a chunked
        body may look like:

        7\r\n
        Mozilla\r\n 
        9\r\n
        Developer\r\n
        7\r\n
        Network\r\n
        0\r\n 
        \r\n

        In this case, we want the parsed body buffer to contain actual body only
        (MozillaDeveloperNetwork in the above example).
         */
        /* If the response body found by the parser (pLoc) is not equal to the
        current writable location in the body buffer (_httpsResponse->pBodyCur),
        it indicates that:
        - Either the data is in the header buffer and needs to be copied into the
        body buffer.
        - Or it is a chunked response and the data needs to be moved up in the
        body buffer. */
        if( (pHttpsResponse->pBodyCur + length) <= pHttpsResponse->pBodyEnd )
        {
            if(pHttpsResponse->pBodyCur != (uint8_t*)pLoc)
            {
                memcpy(pHttpsResponse->pBodyCur, pLoc, length);
            }
             pHttpsResponse->pBodyCur += length;
        }
    }
    return 0;
}

/*-----------------------------------------------------------*/

static int _httpParserOnMessageCompleteCallback(http_parser * pHttpParser)
{
    IotLogDebug("Parser: End of the HTTPS message reached.");
    _httpsResponse_t * pHttpsResponse = (_httpsResponse_t *)(pHttpParser->data);
    pHttpsResponse->parserState = PARSER_STATE_BODY_COMPLETE;

    /* This callback is invoked when the complete HTTP response has been received.
    We tell the parser to parse the whole body buffer as opposed to the size of
    the response body. For example, if the size of the body buffer is 1000 but
    the size of the actual body is 500, we tell the parser to parse the whole
    buffer of length 1000. We do zero out the buffer in the beginning so that all
    the buffer after the actual body contains zeros. In this case when the body
    buffer is larger than the actual buffer, http_parser_execute will return
    either HPE_CLOSED_CONNECTION or HPE_INVALID_CONSTANT. */
    return 0;
}

/*-----------------------------------------------------------*/

/* This code prints debugging information and is, therefore, compiled only when
 * log level is set to IOT_LOG_DEBUG. */
#if (LIBRARY_LOG_LEVEL == IOT_LOG_DEBUG)
static int _httpParserOnChunkHeaderCallback(http_parser * pHttpParser)
{
    ( void )pHttpParser;
    IotLogDebug("Parser: HTTPS message Chunked encoding header callback.");
    IotLogDebug( "Parser: HTTPS message Chunk size: %d", pHttpParser->content_length );
    return 0;
}

/*-----------------------------------------------------------*/

static int _httpParserOnChunkCompleteCallback(http_parser * pHttpParser)
{
    ( void )pHttpParser;
    IotLogDebug("End of a HTTPS message Chunk complete callback.");
    return 0;
}
#endif

/*-----------------------------------------------------------*/

static void _networkReceiveCallback( void* pNetworkConnection, void* pReceiveContext )
{
    _httpsConnection_t* pHttpsConnection = ( _httpsConnection_t* )pReceiveContext;
    ( void )pNetworkConnection;

    /* Post that any thread in the connection can read on the socket. */
    IotSemaphore_Post( &( pHttpsConnection->rxStartSem ) );
    /* Block while that thread reads from the socket. */
    IotSemaphore_Wait( &( pHttpsConnection->rxFinishSem ) );
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _createHttpsConnection(IotHttpsConnectionHandle_t * pConnHandle, IotHttpsConnectionInfo_t *pConnInfo)
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    IotNetworkError_t networkStatus = IOT_NETWORK_SUCCESS;
    /* The maximum string length of the ALPN protocols is configured in IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH. 
       The +1 is for the NULL terminator needed by IotNetworkCredentials_t.pAlpnProtos. */
    char pAlpnProtos[IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH + 1] = { 0 };
    /* The maximum string length of the Server host name is configured in IOT_HTTPS_MAX_HOST_NAME_LENGTH. 
       This +1 is for the NULL terminator needed by IotNetworkServerInfo_t.pHostName. */
    char pHostName[IOT_HTTPS_MAX_HOST_NAME_LENGTH + 1] = { 0 };
    bool connSemCreated = false;
    bool rxStartSemCreated = false;
    bool rxFinishedSemCreated = false;
    IotNetworkServerInfo_t networkServerInfo = { 0 };
    IotNetworkCredentials_t networkCredentials = { 0 };
    _httpsConnection_t *pHttpsConnection = NULL;

    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pConnInfo->userBuffer.pBuffer);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pConnInfo->pNetworkInterface);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pConnInfo->pAddress);
    HTTPS_ON_ARG_ERROR_GOTO_CLEANUP(pConnInfo->addressLen > 0);

    /* Make sure the connection context can fit in the user buffer. */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP(pConnInfo->userBuffer.bufferLen >= connectionUserBufferMinimumSize, 
        IOT_HTTPS_INSUFFICIENT_MEMORY,  
        "Buffer size is too small to initialize the connection context. User buffer size: %d, required minimum size; %d.",
        (*pConnInfo).userBuffer.bufferLen, 
        connectionUserBufferMinimumSize);

    /* Make sure that the server address does not exceed the maximum permitted length. */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP(pConnInfo->addressLen <= IOT_HTTPS_MAX_HOST_NAME_LENGTH,
        IOT_HTTPS_INVALID_PARAMETER,
        "IotHttpsConnectionInfo_t.addressLen has a host name length %d that exceeds maximum length %d.",
        pConnInfo->addressLen,
        IOT_HTTPS_MAX_HOST_NAME_LENGTH);

    /* Make sure that the ALPN protocols does not exceed the maximum permitted length. */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP(pConnInfo->alpnProtocolsLen <= IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH,
        IOT_HTTPS_INVALID_PARAMETER,
        "IotHttpsConnectionInfo_t.alpnProtocolsLen of %d exceeds the configured maximum protocol length %d. See IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH for more information.",
        pConnInfo->alpnProtocolsLen,
        IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH);

    pHttpsConnection = (_httpsConnection_t *)(pConnInfo->userBuffer.pBuffer);
    
    /* Start with the disconnected state. */
    pHttpsConnection->isConnected = false;

    /* This timeout is used to wait for a response on the connection as well as
    for the timeout for the connect operation. */
    if( pConnInfo->timeout == 0 )
    {
        pHttpsConnection->timeout = IOT_HTTPS_RESPONSE_WAIT_MS;
    }
    else
    {
        pHttpsConnection->timeout = pConnInfo->timeout;
    }

    /* pNetworkInterface contains the connect, disconnect, send, and receive functions. */
    pHttpsConnection->pNetworkInterface = pConnInfo->pNetworkInterface;
    
    /* Copy the host name configuration from the address in the connection information. */
    memcpy( pHostName, pConnInfo->pAddress, pConnInfo->addressLen );
    pHostName[pConnInfo->addressLen] = '\0';
    /* Set it in the IOT network abstractions server information parameter. */
    networkServerInfo.pHostName = pHostName; /* This requires a NULL terminated string. */
    networkServerInfo.port = pConnInfo->port;

    /* If this is TLS connection, then set the network credentials. */
    if( ( pConnInfo->flags & IOT_HTTPS_IS_NON_TLS_FLAG ) == 0 )
    {
        if( pConnInfo->flags & IOT_HTTPS_DISABLE_SNI )
        {
            networkCredentials.disableSni = true;
        }
        else
        {
            networkCredentials.disableSni = false;
        }

        
        if( pConnInfo->pAlpnProtocols != NULL )
        {
            memcpy( pAlpnProtos, pConnInfo->pAlpnProtocols, pConnInfo->alpnProtocolsLen );
            pAlpnProtos[pConnInfo->alpnProtocolsLen] = '\0';
            networkCredentials.pAlpnProtos = pAlpnProtos; /* This requires a NULL termination. It is inconsistent with other members in the struct. */
        }
        else
        {
            networkCredentials.pAlpnProtos = NULL;
        }

        /* If any of these are NULL a network error will result depending on the connection. */
        networkCredentials.pRootCa = pConnInfo->pCaCert;
        networkCredentials.rootCaSize = pConnInfo->caCertLen;
        networkCredentials.pClientCert = pConnInfo->pClientCert;
        networkCredentials.clientCertSize = pConnInfo->clientCertLen;
        networkCredentials.pPrivateKey = pConnInfo->pPrivateKey;
        networkCredentials.privateKeySize = pConnInfo->privateKeyLen;
    }

    /* If this is a TLS connection connect with networkCredentials. Otherwise pass NULL. */
    if( ( pConnInfo->flags & IOT_HTTPS_IS_NON_TLS_FLAG ) == 0 )
    {
        /* create() will connect to the server specified. */
        networkStatus = pHttpsConnection->pNetworkInterface->create( &networkServerInfo,
            &networkCredentials,
            &( pHttpsConnection->pNetworkConnection ) );
    }
    else
    {
        networkStatus = pHttpsConnection->pNetworkInterface->create( &networkServerInfo,
            NULL,
            &( pHttpsConnection->pNetworkConnection ) );
    }

    /* Check to see if the network connection succeeded. If it did not succeed,
       then the output parameter pConnHandle will be used to return NULL and the
       function returns an error. */
    if( networkStatus != IOT_NETWORK_SUCCESS )
    {
        IotLogError( "Failed to connect to the server at %.*s on port %d with error: %d",
            pConnInfo->addressLen,
            pConnInfo->pAddress,
            pConnInfo->port,
            networkStatus );
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_CONNECTION_ERROR);
    }
    else
    {
        /* The connection succeeded so set the state to connected. */
        pHttpsConnection->isConnected = true;
    }

    /* The receive callback is invoked by the network layer when data is ready
      to be read from the network. */
    networkStatus = pHttpsConnection->pNetworkInterface->setReceiveCallback( pHttpsConnection->pNetworkConnection,
        _networkReceiveCallback,
        pHttpsConnection );
    if( networkStatus != IOT_NETWORK_SUCCESS )
    {
        IotLogError( "Failed to connect to set the HTTPS receive callback. " );
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    /* Connection was successful, so create semaphores. */

    /* The overall lock on the connection is set to 1 to start so that the first
       thread to need to use it can use it. */
    connSemCreated = IotSemaphore_Create( &( pHttpsConnection->connSem ), 1 /* initialValue */, 1 /* maxValue */ );
    if(!connSemCreated)
    {
        IotLogError("Failed to create an internal connection semaphore.");
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    /* The reading start semaphore is initialized to 0 so that a thread reads
    from the socket only after it has been signaled to do so from the receive
    callback. */
    rxStartSemCreated = IotSemaphore_Create( &( pHttpsConnection->rxStartSem ), 0 /* initialValue */, 1 /* maxValue */ );
    if(!rxStartSemCreated)
    {
        IotLogError( "Failed to create an internal connection semaphore." );
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    /* The reading finished semaphore is initialized to 0 that the application context
       can set it to 1 for the network context to return from the read ready callback. */
    rxFinishedSemCreated = IotSemaphore_Create( &( pHttpsConnection->rxFinishSem ), 0 /* initialValue */, 1 /* maxValue */ );
    if(!rxFinishedSemCreated)
    {
        IotLogError( "Failed to create an internal connection semaphore." );
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    /* Return the new connection information. */
    *pConnHandle = pHttpsConnection;

    HTTPS_FUNCTION_CLEANUP_BEGIN();

    /* If we failed anywhere in the connection process, then destroy the semaphores created. */
    if( HTTPS_FAILED( status ) )
    {
        /* If there was a connect was successful, disconnect from the network.  */
        if(( pHttpsConnection != NULL) && (pHttpsConnection->isConnected))
        {   
            _networkDisconnect( pHttpsConnection );
            _networkDestroy( pHttpsConnection );
        }

        if(connSemCreated)
        {
            IotSemaphore_Destroy( &(pHttpsConnection->connSem) );
        }

        if(rxStartSemCreated)
        {
            IotSemaphore_Destroy( &(pHttpsConnection->rxStartSem) );
        }

        if(rxFinishedSemCreated)
        {
            IotSemaphore_Destroy( &(pHttpsConnection->rxFinishSem) );
        }

        

        /* Return NUll in the output paramteter if anything failed. */
        *pConnHandle = NULL;
    }

    HTTPS_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

static void _networkDisconnect(_httpsConnection_t* pHttpsConnection)
{
    IotNetworkError_t networkStatus = IOT_NETWORK_SUCCESS;

    networkStatus = pHttpsConnection->pNetworkInterface->close( pHttpsConnection->pNetworkConnection );
    if ( networkStatus != IOT_NETWORK_SUCCESS )
    {
        IotLogWarn("Failed to shutdown the socket with error code: %d", networkStatus );
    }
}

/*-----------------------------------------------------------*/

static void _networkDestroy(_httpsConnection_t* pHttpsConnection)
{
    IotNetworkError_t networkStatus = IOT_NETWORK_SUCCESS;

    networkStatus = pHttpsConnection->pNetworkInterface->destroy( pHttpsConnection->pNetworkConnection );
    if ( networkStatus != IOT_NETWORK_SUCCESS )
    {
        IotLogWarn("Failed to shutdown the socket with error code: %d", networkStatus );
    }
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _addHeader(_httpsRequest_t * pHttpsRequest, const char * pName, const char * pValue, uint32_t valueLen )
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    int nameLen = strlen( pName ) ;
    int headerFieldSeparatorLen = HTTPS_HEADER_FIELD_SEPARATOR_LENGTH;
    uint32_t additionalLength = nameLen + headerFieldSeparatorLen + valueLen + HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;
    uint32_t possibleLastHeaderAdditionalLength = HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;

    /* Check if there is enough space to add the header field and value
    (name:value\r\n). We need to add a "\r\n" at the end of headers. The use of
    possibleLastHeaderAdditionalLength is to make sure that there is always
    space for the last "\r\n". */
    if( (additionalLength + possibleLastHeaderAdditionalLength + pHttpsRequest->pHeadersCur) > (pHttpsRequest->pHeadersEnd) )
    {
        IotLogError( "There is %d space left in the header buffer, but we want to add %d more of header.",
            pHttpsRequest->pHeadersEnd - pHttpsRequest->pHeadersCur,
            additionalLength + possibleLastHeaderAdditionalLength );
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INSUFFICIENT_MEMORY);
    }

    memcpy(pHttpsRequest->pHeadersCur, pName, nameLen);
    pHttpsRequest->pHeadersCur += nameLen;
    memcpy(pHttpsRequest->pHeadersCur, HTTPS_HEADER_FIELD_SEPARATOR, headerFieldSeparatorLen);
    pHttpsRequest->pHeadersCur += headerFieldSeparatorLen;
    memcpy(pHttpsRequest->pHeadersCur, pValue, valueLen);
    pHttpsRequest->pHeadersCur += valueLen;
    memcpy(pHttpsRequest->pHeadersCur, HTTPS_END_OF_HEADER_LINES_INDICATOR, HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH);
    pHttpsRequest->pHeadersCur += HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;
    IotLogDebug("Wrote header: \"%s: %.*s\r\n\". Space left in request user buffer: %d", 
        pName,
        valueLen,
        pValue,
        pHttpsRequest->pHeadersEnd - pHttpsRequest->pHeadersCur);

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _networkSend(_httpsConnection_t* pHttpsConnection, uint8_t * pBuf, size_t len)
{
    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    size_t numBytesSent = 0;
    size_t numBytesSentTotal = 0;
    size_t sendLength = len;

    while( numBytesSentTotal < sendLength )
    {
        numBytesSent = pHttpsConnection->pNetworkInterface->send( pHttpsConnection->pNetworkConnection, 
            &( pBuf[numBytesSentTotal] ), 
            sendLength - numBytesSentTotal );

        /* pNetworkInterface->send returns 0 on error. */
        if( numBytesSent == 0 )
        {
            IotLogError("Error in sending the HTTPS headers. Error code: %d", numBytesSent);
            break;
        }

        numBytesSentTotal += numBytesSent;
    }

    if( numBytesSentTotal != sendLength )
    {
        IotLogError("Error sending data on the network. We sent %d but there were total %d.", numBytesSentTotal, sendLength);
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_NETWORK_ERROR);
    }

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _networkRecv( _httpsConnection_t* pHttpsConnection, uint8_t * pBuf, size_t bufLen)
{
    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    size_t numBytesRecv = 0;

    numBytesRecv = pHttpsConnection->pNetworkInterface->receive( pHttpsConnection->pNetworkConnection,
        pBuf,
        bufLen);

    /* We return IOT_HTTPS_NETWORK_ERROR only if we receive nothing. Receiving less
       data than requested is okay because it is not known in advance how much data
       we are going to receive and therefore we request for the available buffer
       size. */
    if( numBytesRecv == 0)
    {
        IotLogError("Error in receiving the HTTPS response message. Socket Error code %d", numBytesRecv);
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_TIMEOUT_ERROR);
    }

    HTTPS_FUNCTION_EXIT_NO_CLEANUP(); 
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _sendHttpsHeaders( _httpsConnection_t* pHttpsConnection, uint8_t* pHeadersBuf, uint32_t headersLength, bool isNonPersistent, uint32_t contentLength)
{
    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    const char* connectionHeader = NULL;
    int numWritten = 0;
    int connectionHeaderLen = 0;
    /* The Content-Length header of the form "Content-Length: N\r\n" with a NULL terminator for snprintf. */
    char contentLengthHeaderStr[HTTPS_MAX_CONTENT_LENGTH_LINE_LENGTH + 1];
    /* The HTTP headers to send after the headers in pHeadersBuf are the Content-Length and the Connection type and
       the final "\r\n" to indicate the end of the the header lines. Note that we are using
       HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE_LENGTH because length of "Connection: keep-alive\r\n" is
       more than "Connection: close\r\n". Creating a buffer of bigger size ensures that
       both the connection type strings will fit in the buffer. */
    char finalHeaders[HTTPS_MAX_CONTENT_LENGTH_LINE_LENGTH + HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE_LENGTH + HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH] = { 0 };
    
    /* Send the headers passed into this function first. These headers are not termined with a second set of "\r\n". */
    status = _networkSend( pHttpsConnection, pHeadersBuf, headersLength);
    if( HTTPS_FAILED( status ) )
    {
        IotLogError("Error sending the HTTPS headers in the request user buffer. Error code: %d", status);
        HTTPS_GOTO_CLEANUP();
    }

    /* If there is a Content-Length, then write that to the finalHeaders to send. */
    if (contentLength > 0)
    {
        numWritten = snprintf(contentLengthHeaderStr, 
            sizeof(contentLengthHeaderStr), 
            "%s: %d\r\n",
            HTTPS_CONTENT_LENGTH_HEADER,
            contentLength);
    }
    if( numWritten < 0 )
    {
        IotLogError("Internal error in snprintf() in _sendHttpsHeaders(). Error code %d.", numWritten);
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    /* snprintf() succeeded so copy that to the finalHeaders. */
    memcpy(finalHeaders, contentLengthHeaderStr, numWritten);
    /* Write the connection persistence type to the final headers. */
    if (isNonPersistent)
    {
        connectionHeader = HTTPS_CONNECTION_CLOSE_HEADER_LINE;
        connectionHeaderLen = FAST_MACRO_STRLEN(HTTPS_CONNECTION_CLOSE_HEADER_LINE);
    }
    else
    {
        connectionHeader = HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE;
        connectionHeaderLen = FAST_MACRO_STRLEN(HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE);
    }
    memcpy(&finalHeaders[numWritten], connectionHeader, connectionHeaderLen);
    numWritten += connectionHeaderLen;
    memcpy( &finalHeaders[numWritten], HTTPS_END_OF_HEADER_LINES_INDICATOR, HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH );
    numWritten += HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;

    status = _networkSend( pHttpsConnection, (uint8_t*)finalHeaders, numWritten);
    if( HTTPS_FAILED( status ) )
    {
        IotLogError("Error sending final HTTPS Headers \r\n%s. Error code: %d", finalHeaders, status);
        HTTPS_GOTO_CLEANUP();
    }

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _sendHttpsBody( _httpsConnection_t* pHttpsConnection, uint8_t* pBodyBuf, uint32_t bodyLength)
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    status = _networkSend( pHttpsConnection, pBodyBuf, bodyLength);
    if( HTTPS_FAILED( status ) )
    {
        IotLogError("Error sending final HTTPS body at location 0x%x. Error code: %d", pBodyBuf, status);
        HTTPS_GOTO_CLEANUP();
    }

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _parseHttpsMessage(_httpParserInfo_t* pHttpParserInfo, char* pBuf, size_t len)
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    size_t parsedBytes = 0;
    const char * pHttpParserErrorDescription = NULL;
    http_parser *pHttpParser = &(pHttpParserInfo->parser);

    parsedBytes = pHttpParserInfo->parseFunc( pHttpParser, &_httpParserSettings, pBuf, len );
    IotLogDebug( "http-parser parsed %d bytes out of %d specified.", parsedBytes, len );

    /* If the parser fails with HPE_CLOSED_CONNECTION or HPE_INVALID_CONSTANT that simply means there
       was data beyond the end of the message. We do not fail in this case because we give the whole
       header buffer or body buffer to the parser even if it is only partly filled with data.
       Errors <= HPE_CB_chunk_complete means that a non-zero number was returned from some callback.
       A nonzero number is returned from some callbacks when we want to stop the parser early
       for example - a HEAD request or the user explicitly asked to ignore the body by not
       providing the body buffer. */
    if( (pHttpParser->http_errno != 0) &&
        ( HTTP_PARSER_ERRNO( pHttpParser ) != HPE_CLOSED_CONNECTION ) &&
        ( HTTP_PARSER_ERRNO( pHttpParser ) != HPE_INVALID_CONSTANT ) &&
        ( HTTP_PARSER_ERRNO( pHttpParser ) > HPE_CB_chunk_complete) )
    {
        pHttpParserErrorDescription = http_errno_description( HTTP_PARSER_ERRNO( pHttpParser ) );
        IotLogError("http_parser failed on the http response with error: %s", pHttpParserErrorDescription);
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_PARSING_ERROR);
    }

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _receiveHttpsMessage( _httpsConnection_t* pHttpsConnection, 
    _httpParserInfo_t *pHttpParserInfo,
    IotHttpsResponseParserState_t *pCurrentParserState,
    IotHttpsResponseParserState_t finalParserState, 
    uint8_t** pBufCur,
    uint8_t** pBufEnd,
    IotHttpsReturnCode_t *pNetworkStatus)
{    
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    while( (*pCurrentParserState < finalParserState) && (*pBufEnd - *pBufCur > 0) ) 
    {
        *pNetworkStatus = _networkRecv( pHttpsConnection,
            *pBufCur, 
            *pBufEnd - *pBufCur);

        status = _parseHttpsMessage(pHttpParserInfo, (char*)(*pBufCur), *pBufEnd - *pBufCur);
        if(HTTPS_FAILED(status))
        {
            IotLogError("Failed to parse the message buffer with error: %d", pHttpParserInfo->parser.http_errno);
            break;
        }

        /* The _httResponse->pHeadersCur pointer is updated in the http_parser callbacks. */
        IotLogDebug( "There is %d of space left in the buffer.", *pBufEnd - *pBufCur );

        /* A non-ok network status does not necessarily indicate an error because
        we receive a network error if the server closes connection even after
        sending a valid HTTP response. We therefore only return error if a valid
        HTTP response is not received i.e. the parser failed to parse the received
        response. That is the reason that this function returns "status" and not
        *pNetworkStatus. */
        if( HTTPS_FAILED(*pNetworkStatus) )
        {
            IotLogError( "Network error receiving the HTTPS response headers. Error code: %d", pNetworkStatus );
            break;
        }

    }

    /* If we did not reach the end of the headers or body in the parser callbacks, then the buffer configured does not
       fit all of that part of the HTTP message. */
    if( *pCurrentParserState < finalParserState)
    {
        IotLogDebug("There are still more data on the network. It could not fit into the specified length %d.",
            *pBufEnd - *pBufCur);
    }

    HTTPS_GOTO_CLEANUP();
    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _receiveHttpsHeaders( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t *pNetworkStatus)
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    status = _receiveHttpsMessage(pHttpsConnection,
        &(pHttpsResponse->httpParserInfo),
        &(pHttpsResponse->parserState),
        PARSER_STATE_HEADERS_COMPLETE,
        &(pHttpsResponse->pHeadersCur),
        &(pHttpsResponse->pHeadersEnd),
        pNetworkStatus);

    if( HTTPS_FAILED(status))
    {
        IotLogError("Error receiving the HTTP headers. Error code %d", status);
        HTTPS_GOTO_CLEANUP();
    }

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

/* _receiveHttpsHeaders() must be called first before this function is called. */
static IotHttpsReturnCode_t _receiveHttpsBody( _httpsConnection_t* pHttpsConnection, 
                                               _httpsResponse_t* pHttpsResponse, 
                                               IotHttpsReturnCode_t *pNetworkStatus)
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    pHttpsResponse->bufferProcessingState = PROCESSING_STATE_FILLING_BODY_BUFFER;

    status = _receiveHttpsMessage(pHttpsConnection,
        &(pHttpsResponse->httpParserInfo),
        &(pHttpsResponse->parserState),
        PARSER_STATE_BODY_COMPLETE,
        &(pHttpsResponse->pBodyCur),
        &(pHttpsResponse->pBodyEnd),
        pNetworkStatus);
    if( HTTPS_FAILED(status) )
    {
        IotLogError("Error receiving the HTTP body. Error code %d", status);
        HTTPS_GOTO_CLEANUP();
    }

    HTTPS_FUNCTION_CLEANUP_BEGIN();

    IotLogDebug("The message Content-Length is %d (Will be > 0 for a Content-Length header existing). "
        "The remaining content length on the network is %d.",
        pHttpsResponse->contentLength,
        pHttpsResponse->httpParserInfo.parser.content_length);

    HTTPS_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _flushHttpsNetworkData( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse )
{
    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    static uint8_t flushBuffer[IOT_HTTPS_MAX_FLUSH_BUFFER_SIZE] = { 0 };
    const char * pHttpParserErrorDescription = NULL;
    IotHttpsReturnCode_t parserStatus = IOT_HTTPS_OK;
    IotHttpsReturnCode_t networkStatus = IOT_HTTPS_OK;

    /* Even if there is not body, the parser state will become body complete after the headers finish. */
    while( pHttpsResponse->parserState < PARSER_STATE_BODY_COMPLETE )
    {
        IotLogDebug( "Now clearing the rest of the response data on the socket. " );
        networkStatus = _networkRecv( pHttpsConnection, flushBuffer, IOT_HTTPS_MAX_FLUSH_BUFFER_SIZE );

        /* Run this through the parser so that we can get the end of the HTTP message, instead of simply timing out the socket to stop.
           If we relied on the socket timeout to stop reading the network socket, then the server may close the connection. */
        parserStatus = _parseHttpsMessage(&(pHttpsResponse->httpParserInfo), (char*)flushBuffer, IOT_HTTPS_MAX_FLUSH_BUFFER_SIZE );
        if(HTTPS_FAILED(parserStatus))
        {
            pHttpParserErrorDescription = http_errno_description( HTTP_PARSER_ERRNO( &pHttpsResponse->httpParserInfo.parser ) );
            IotLogError("Network Flush: Failed to parse the response body buffer with error: %d", pHttpsResponse->httpParserInfo.parser.http_errno);
            break;
        }

        /* If there is a network error then we want to stop clearing out the buffer. */
        if( HTTPS_FAILED(networkStatus) )
        {
            IotLogError( "Error receiving the HTTPS response headers. Error code: %d", networkStatus );
            break;
        }
    }

    /* All network errors except timeouts are returned. */
    if( networkStatus != IOT_HTTPS_TIMEOUT_ERROR)
    {
        status = networkStatus;
    }
    else
    {
        status = parserStatus;
    }
    HTTPS_GOTO_CLEANUP();

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_Init( void )
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    /* This sets all member in the _httpParserSettings to zero. It does not return any errors. */
    http_parser_settings_init( &_httpParserSettings );

    /* Set the http-parser callbacks. */
    _httpParserSettings.on_message_begin = _httpParserOnMessageBeginCallback;
    _httpParserSettings.on_status = _httpParserOnStatusCallback;
    _httpParserSettings.on_header_field = _httpParserOnHeaderFieldCallback;
    _httpParserSettings.on_header_value = _httpParserOnHeaderValueCallback;
    _httpParserSettings.on_headers_complete = _httpParserOnHeadersCompleteCallback;
    _httpParserSettings.on_body = _httpParserOnBodyCallback;
    _httpParserSettings.on_message_complete = _httpParserOnMessageCompleteCallback;
/* This code prints debugging information and is, therefore, compiled only when
 * log level is set to IOT_LOG_DEBUG. */
#if (LIBRARY_LOG_LEVEL == IOT_LOG_DEBUG)
    _httpParserSettings.on_chunk_header = _httpParserOnChunkHeaderCallback;
    _httpParserSettings.on_chunk_complete = _httpParserOnChunkCompleteCallback;
#endif
    HTTPS_GOTO_CLEANUP();
    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

void IotHttpsClient_Deinit( void )
{
    /* There is nothing to de-initialize here as of now. */
}

/* --------------------------------------------------------- */

IotHttpsReturnCode_t IotHttpsClient_Connect(IotHttpsConnectionHandle_t * pConnHandle, IotHttpsConnectionInfo_t *pConnInfo)
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    /* Check for NULL parameters in a public API. */
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pConnHandle);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pConnInfo);

    /* If a valid connection handle is passed in. */
    if(*pConnHandle != NULL)
    {
        /* If the handle in a connected state, then we want to disconnect before reconnecting. The ONLY way to put the 
           handle is a disconnect state is to call IotHttpsClient_Disconnect(). */
        if((*pConnHandle)->isConnected)
        {
            status = IotHttpsClient_Disconnect(*pConnHandle);
            if(HTTPS_FAILED(status))
            {
                IotLogError("Error disconnecting a connected *pConnHandle passed to IotHttpsClient_Connect().Error code %d", status);
                *pConnHandle = NULL;
                HTTPS_GOTO_CLEANUP();
            }
        }
    }

    /* Connect to the server now. Initialize all resources needed for the connection context as well here. */
    status = _createHttpsConnection(pConnHandle, pConnInfo);
    if(HTTPS_FAILED(status))
    {
        IotLogError("Error in IotHttpsClient_Connect(). Error code %d.", status);
    }

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_Disconnect(IotHttpsConnectionHandle_t connHandle)
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( connHandle );

    /* Do not attempt to disconnect an already disconnected connection.
    It can happen when a user calls this functions and we return
    IOT_HTTPS_BUSY. */
    if( connHandle->isConnected == false )
    {
        /* Mark the network as disconnected whether the disconnect passes or not. */
        connHandle->isConnected = false;
        
        /* Disconnect from the network. */
        _networkDisconnect(connHandle);
    }

    if( IotSemaphore_TryWait(&(connHandle->connSem) ) == false)
    {
        IotLogError("Connection is in use, cannot destroy yet. Please wait for the outstanding request to finish.");
        /* The user is expected to call the same function again to ensure
           when we return IOT_HTTPS_BUSY so that the associated resources are
           released. */
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_BUSY);
    }

    /* Clean connection context resources. */
    IotSemaphore_Destroy( &(connHandle->connSem) );
    IotSemaphore_Destroy( &(connHandle->rxStartSem) );
    IotSemaphore_Destroy( &(connHandle->rxFinishSem) );

    /* Destroy the network connection (cleaning up network socket resources). */
    _networkDestroy(connHandle);

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_InitializeRequest(IotHttpsRequestHandle_t * pReqHandle, IotHttpsRequestInfo_t *pReqInfo)
{
    HTTPS_FUNCTION_ENTRY( IOT_HTTPS_OK );

    _httpsRequest_t * pHttpsRequest = NULL;
    size_t additionalLength = 0;
    size_t spaceLen = 1;
    char* pSpace = " ";
    size_t httpsMethodLen = 0;
    size_t httpsProtocolVersionLen = FAST_MACRO_STRLEN(HTTPS_PROTOCOL_VERSION);

    /* Check for NULL parameters in the public API. */
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( pReqHandle );
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( pReqInfo );
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( pReqInfo->reqUserBuffer.pBuffer );
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( pReqInfo->pHost );
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( pReqInfo->pSyncInfo );
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( pReqInfo->respUserBuffer.pBuffer );

    /* Check of the user buffer is large enough for the request context + default headers. */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP( pReqInfo->reqUserBuffer.bufferLen >= requestUserBufferMinimumSize,
        IOT_HTTPS_INSUFFICIENT_MEMORY,
        "Buffer size is too small to initialize the request context. User buffer size: %d, required minimum size; %d.",
        pReqInfo->reqUserBuffer.bufferLen,
        requestUserBufferMinimumSize);

    /* Check of the user buffer is large enough for the response context + default headers. */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP( pReqInfo->respUserBuffer.bufferLen >= responseUserBufferMinimumSize,
        IOT_HTTPS_INSUFFICIENT_MEMORY,
        "Buffer size is too small to initialize the response context. User buffer size: %d, required minimum size; %d.",
        pReqInfo->respUserBuffer.bufferLen, 
        responseUserBufferMinimumSize);

    /* Set the request contet to the start of the userbuffer. */
    pHttpsRequest = ( _httpsRequest_t *)(pReqInfo->reqUserBuffer.pBuffer);
    /* Clear out the user buffer. */
    memset(pReqInfo->reqUserBuffer.pBuffer, 0, pReqInfo->reqUserBuffer.bufferLen);

    /* Set the start of the headers to the end of the request context in the user buffer. */
    pHttpsRequest->pHeaders = (uint8_t*)pHttpsRequest + sizeof( _httpsRequest_t );
    pHttpsRequest->pHeadersEnd = (uint8_t*)pHttpsRequest + pReqInfo->reqUserBuffer.bufferLen;
    pHttpsRequest->pHeadersCur = pHttpsRequest->pHeaders;

    /* Get the length of the HTTP method. */
    httpsMethodLen = strlen( _pHttpsMethodStrings[pReqInfo->method] );

    /* Add the request line to the header buffer. */
    additionalLength = httpsMethodLen + \
        spaceLen + \
        pReqInfo->pathLen + \
        spaceLen + \
        httpsProtocolVersionLen + \
        HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;
    if( (additionalLength + pHttpsRequest->pHeadersCur) > (pHttpsRequest->pHeadersEnd ))
    {
        IotLogError("Request line does not fit into the request user buffer: \"%s %.*s HTTP/1.1\\r\\n\" . ",
            _pHttpsMethodStrings[IOT_HTTPS_METHOD_GET],
            pReqInfo->pathLen,
            pReqInfo->pPath);
        IotLogError( "The length needed is %d and the space available is %d.", additionalLength, pHttpsRequest->pHeadersEnd - pHttpsRequest->pHeadersCur );
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INSUFFICIENT_MEMORY);
    }

    /* Write "<METHOD> <PATH> HTTP/1.1\r\n" to the start of the header space. */
    memcpy(pHttpsRequest->pHeadersCur, _pHttpsMethodStrings[pReqInfo->method], httpsMethodLen);
    pHttpsRequest->pHeadersCur += httpsMethodLen;
    memcpy(pHttpsRequest->pHeadersCur, pSpace, spaceLen);
    pHttpsRequest->pHeadersCur += spaceLen;
    if(pReqInfo->pPath == NULL)
    {
        pReqInfo->pPath = HTTPS_EMPTY_PATH;
        pReqInfo->pathLen = FAST_MACRO_STRLEN(HTTPS_EMPTY_PATH);
    }
    memcpy(pHttpsRequest->pHeadersCur, pReqInfo->pPath, pReqInfo->pathLen);
    pHttpsRequest->pHeadersCur += pReqInfo->pathLen;
    memcpy( pHttpsRequest->pHeadersCur, pSpace, spaceLen );
    pHttpsRequest->pHeadersCur += spaceLen;
    memcpy(pHttpsRequest->pHeadersCur, HTTPS_PROTOCOL_VERSION, httpsProtocolVersionLen);
    pHttpsRequest->pHeadersCur += httpsProtocolVersionLen;
    memcpy(pHttpsRequest->pHeadersCur, HTTPS_END_OF_HEADER_LINES_INDICATOR, HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH);
    pHttpsRequest->pHeadersCur += HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;

    /* Add the User-Agent header. */
    status = _addHeader(pHttpsRequest, "User-Agent", IOT_HTTPS_USER_AGENT, FAST_MACRO_STRLEN( IOT_HTTPS_USER_AGENT ));
    if( HTTPS_FAILED(status) )
    {
        IotLogError("Failed to write header to the request user buffer: \"User-Agent: %s\\r\\n\" . Error code: %d", 
            IOT_HTTPS_USER_AGENT, 
            status);
        HTTPS_GOTO_CLEANUP();
    }

    status = _addHeader(pHttpsRequest, "Host", pReqInfo->pHost, pReqInfo->hostLen);
    if( HTTPS_FAILED(status) )
    {
        IotLogError("Failed to write \"Host: %.*s\\r\\n\" to the request user buffer. Error code: %d", 
            pReqInfo->hostLen, 
            pReqInfo->pHost, 
            status);
        HTTPS_GOTO_CLEANUP();
    }

    /* Set the HTTP request entity body. This is allowed to be NULL for no body like for a GET request. */
    pHttpsRequest->pBody = pReqInfo->pSyncInfo->pReqData;
    pHttpsRequest->bodyLength = pReqInfo->pSyncInfo->reqDataLen;

    /* Save the connection info if the connection is to be made at the time of the request. */
    pHttpsRequest->pConnInfo = pReqInfo->pConnInfo;
    /* Set the connection persistence flag for keeping the connection open after receiving a response. */
    pHttpsRequest->isNonPersistent = pReqInfo->isNonPersistent;

    /* Initialize the corresponding response to this request. */
    pHttpsRequest->pHttpsResponse = (_httpsResponse_t *)(pReqInfo->respUserBuffer.pBuffer);
    /* Clear out the response user buffer. This is important because we
    give the whole buffer to the parser as opposed to the actual content
    length and rely on the parser to stop when a complete HTTP response
    is found. To make sure that any data in the buffer which is not part
    of the received HTTP response, does not get interpreted as part of
    the HTTP repose, we zero out the buffer here. */
    memset(pReqInfo->respUserBuffer.pBuffer, 0, pReqInfo->respUserBuffer.bufferLen);
        
    pHttpsRequest->pHttpsResponse->pHeaders = (uint8_t*)(pHttpsRequest->pHttpsResponse) + sizeof(_httpsResponse_t);
    pHttpsRequest->pHttpsResponse->pHeadersEnd = (uint8_t*)(pHttpsRequest->pHttpsResponse) + pReqInfo->respUserBuffer.bufferLen;
    pHttpsRequest->pHttpsResponse->pHeadersCur = pHttpsRequest->pHttpsResponse->pHeaders;

    /* The request body pointer is allowed to be NULL. pSyncInfo was checked for NULL earlier in this function. */
    pHttpsRequest->pHttpsResponse->pBody = pReqInfo->pSyncInfo->pRespData;
    pHttpsRequest->pHttpsResponse->pBodyCur = pHttpsRequest->pHttpsResponse->pBody;
    pHttpsRequest->pHttpsResponse->pBodyEnd = pHttpsRequest->pHttpsResponse->pBody + pReqInfo->pSyncInfo->respDataLen;
    /* Clear out the body bufffer. This is important because we give the
       whole buffer to the parser as opposed to the actual content length and
       rely on the parser to stop when a complete HTTP response is found. To
       make sure that any data in the buffer which is not part of the received
       HTTP response, does not get interpreted as part of the HTTP repose, we
       zero out the buffer here. */
    memset(pReqInfo->pSyncInfo->pRespData, 0, pReqInfo->pSyncInfo->respDataLen);

    /* Reinitialize the parser and set the fill buffer state to empty. This does not return any errors. */
    http_parser_init(&(pHttpsRequest->pHttpsResponse->httpParserInfo.parser), HTTP_RESPONSE);
    /* Set the third party http parser function. */
    pHttpsRequest->pHttpsResponse->httpParserInfo.parseFunc = http_parser_execute;

    pHttpsRequest->pHttpsResponse->status = 0;
    pHttpsRequest->pHttpsResponse->method = pReqInfo->method;
    pHttpsRequest->pHttpsResponse->contentLength = 0;
    pHttpsRequest->pHttpsResponse->parserState = PARSER_STATE_NONE;
    pHttpsRequest->pHttpsResponse->bufferProcessingState = PROCESSING_STATE_NONE;
    pHttpsRequest->pHttpsResponse->pReadHeaderField = NULL;
    pHttpsRequest->pHttpsResponse->pReadHeaderValue = NULL;
    pHttpsRequest->pHttpsResponse->readHeaderValueLength = 0;
    pHttpsRequest->pHttpsResponse->foundHeaderField = 0;
    pHttpsRequest->pHttpsResponse->pHttpsConnection = NULL;
    *pReqHandle = pHttpsRequest;

    HTTPS_FUNCTION_CLEANUP_BEGIN();
    if((status != IOT_HTTPS_OK) && (pReqHandle != NULL))
    {
        /* Set the request handle to return to NULL, if we failed anywhere. */
        *pReqHandle = NULL;
    }
    HTTPS_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_AddHeader( IotHttpsRequestHandle_t reqHandle, char * pName, char * pValue, uint32_t len )
{
    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    /* Check for NULL pointer paramters. */
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( pName );
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( pValue );
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP( reqHandle );

    /* Check for auto-generated header "Content-Length". This header is created and send automatically when right before
       request body is sent on the network. */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP(strncmp(pName, HTTPS_CONTENT_LENGTH_HEADER, FAST_MACRO_STRLEN(HTTPS_CONTENT_LENGTH_HEADER)) != 0,
        IOT_HTTPS_INVALID_PARAMETER,
        "Attempting to add auto-generated header %s. This is not allowed.",
        HTTPS_CONTENT_LENGTH_HEADER);

    /* Check for auto-generated header "Connection". This header is created and send automatically when right before
       request body is sent on the network. */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP(strncmp(pName, HTTPS_CONNECTION_HEADER, FAST_MACRO_STRLEN(HTTPS_CONNECTION_HEADER)) != 0,
        IOT_HTTPS_INVALID_PARAMETER,
        "Attempting to add auto-generated header %s. This is not allowed.", 
        HTTPS_CONNECTION_HEADER);

        /* Check for auto-generated header "Host". This header is created and placed into the header buffer space
        in IotHttpClient_InitializeRequest(). */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP(strncmp(pName, HTTPS_HOST_HEADER, FAST_MACRO_STRLEN(HTTPS_HOST_HEADER)) != 0,
        IOT_HTTPS_INVALID_PARAMETER,
        "Attempting to add auto-generated header %s. This is not allowed.", 
        HTTPS_HOST_HEADER);

        /* Check for auto-generated header "User-Agent". This header is created and placed into the header buffer space
        in IotHttpClient_InitializeRequest(). */
    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP(strncmp(pName, HTTPS_USER_AGENT_HEADER, FAST_MACRO_STRLEN(HTTPS_USER_AGENT_HEADER)) != 0,
        IOT_HTTPS_INVALID_PARAMETER,
        "Attempting to add auto-generated header %s. This is not allowed.", 
        HTTPS_USER_AGENT_HEADER);


    status = _addHeader(reqHandle, pName, pValue, len );
    if(HTTPS_FAILED(status))
    {
        IotLogError("Error in IotHttpsClient_AddHeader(), error code %d.", status);
        HTTPS_GOTO_CLEANUP();
    }

    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_SendSync(IotHttpsConnectionHandle_t *pConnHandle, IotHttpsRequestHandle_t reqHandle, IotHttpsResponseHandle_t * pRespHandle)
{

    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    _httpsResponse_t * pHttpsResponse = NULL;
    _httpsConnection_t * pHttpsConnection = NULL;
    IotHttpsReturnCode_t flushStatus = IOT_HTTPS_OK;
    IotHttpsReturnCode_t networkStatus = IOT_HTTPS_OK;
    IotHttpsReturnCode_t disconnectStatus = IOT_HTTPS_OK;

    /* Parameter checks. */
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pConnHandle);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(reqHandle);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pRespHandle);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(reqHandle->pHttpsResponse);

    /* Set the internal response context to use and the response handle to return. */
    pHttpsResponse = reqHandle->pHttpsResponse;

    /* Set the response handle to return. */
    *pRespHandle = pHttpsResponse;
    ( *pRespHandle )->pHttpsRequest = reqHandle;

    /* If the pConnHandle points to a NULL handle or the pConnHandle is false, then make the connection now. */
    if( (* pConnHandle == NULL) || ((* pConnHandle)->isConnected == false ))
    {
        /* In order to make the connection now the pConnInfo member of IotHttpsRequestHandle_t must not be NULL. This 
           can be NULL for non-implicit connection, so we perform the NULL check here. */
        HTTPS_ON_NULL_ARG_GOTO_CLEANUP( reqHandle->pConnInfo );

        /* This routine will set the pConnHandle to return if successful. */
        status = _createHttpsConnection(pConnHandle, reqHandle->pConnInfo);
        if(HTTPS_FAILED(status))
        {
            IotLogError("An error occurred in creating the HTTPS connection with error code: %d", status);
            IOT_GOTO_CLEANUP();
        }
    }
    /* Else pConnHandle is not null and it is connected. */

    /* Reset the http-parser state to an initial state. This is done so that a new response can be parsed from the 
       beginning. */
    pHttpsResponse->httpParserInfo.parser.data = (void *)(pHttpsResponse);
    pHttpsResponse->parserState = PARSER_STATE_NONE;
    pHttpsResponse->bufferProcessingState = PROCESSING_STATE_FILLING_HEADER_BUFFER;

    /* Set the internal connection context since we are connected now. */
    pHttpsConnection = *pConnHandle;

    /* Lock the entire connection for sending the request and receiving the response. */
    if( IotSemaphore_TimedWait( &(pHttpsConnection->connSem), IOT_HTTPS_MAX_CONN_USAGE_WAIT_MS ) == false )
    {
        IotLogError("Timed out in sync send waiting on the connection handle to be free. The current timeout is %d.", IOT_HTTPS_MAX_CONN_USAGE_WAIT_MS);
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_BUSY);
    }

    /* Send the headers first. Because the HTTPS body is in a separate pointer. */
    status = _sendHttpsHeaders( pHttpsConnection,
        reqHandle->pHeaders,
        reqHandle->pHeadersCur - reqHandle->pHeaders,
        reqHandle->isNonPersistent,
        reqHandle->bodyLength);

    if( HTTPS_FAILED(status) )
    {
        IotLogError("Error sending the HTTPS headers in the request user buffer. Error code: %d", status);
        HTTPS_GOTO_CLEANUP();
    }

    /* Send the body now if it exists. */
    if(reqHandle->pBody != NULL)
    {
        status = _sendHttpsBody( pHttpsConnection, reqHandle->pBody, reqHandle->bodyLength);
        if( HTTPS_FAILED(status) )
        {
            IotLogError("Error sending final HTTPS body. Return code: %d", status);
            HTTPS_GOTO_CLEANUP(); 
        }
    }

    /* Wait for the network to have data to read.*/
    if( IotSemaphore_TimedWait( &( pHttpsConnection->rxStartSem ), pHttpsConnection->timeout ) == false )
    {
        IotLogError( "Timed out waiting for a response from the network." );
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_TIMEOUT_ERROR);
    }

    status = _receiveHttpsHeaders( pHttpsConnection, pHttpsResponse, &networkStatus);
    if(HTTPS_FAILED(status))
    {
        IotLogError("Error in receiving the HTTPS headers into user header buffer at 0x%x. Error code: %d. Network error %d.", 
            pHttpsResponse->pHeaders, 
            status, 
            networkStatus);
        HTTPS_GOTO_CLEANUP(); 
    }

    if( pHttpsResponse->parserState < PARSER_STATE_HEADERS_COMPLETE )
    {
        IotLogDebug( "Headers received on the network did not all fit into the configured header buffer. "
            "The length of the headers buffer is: %d",
            pHttpsResponse->pHeadersEnd - pHttpsResponse->pHeaders );
        /* It is not error if the headers did not all fit into the buffer. */
    }

    /* The header buffer is now filled or the end of the headers has been reached already. If part of the response
        body was read from the network into the header buffer, then it was already copied to the body buffer in the 
        _httpParserOnBodyCallback(). */
    if(pHttpsResponse->pBody != NULL)
    {
        /* If there is room left in the body buffer and we have not received the whole response body, 
           then try to receive more. */
        if( ( (pHttpsResponse->pBodyEnd - pHttpsResponse->pBodyCur) > 0 ) && 
            ( pHttpsResponse->parserState < PARSER_STATE_BODY_COMPLETE ) )
        {
            status = _receiveHttpsBody( pHttpsConnection,
                pHttpsResponse,
                &networkStatus );
            if( HTTPS_FAILED(status) )
            {
                IotLogError( "Error receiving the HTTPS response body. Error code: %d. Network status %d.", 
                    status, 
                    networkStatus );
                HTTPS_GOTO_CLEANUP();
            }
        }

        /* If we don't reach the end of the HTTPS body in the parser, then we only received part of the body.
            The rest of body will be on the socket. */
        if( pHttpsResponse->parserState < PARSER_STATE_BODY_COMPLETE )
        {
            IotLogError( "HTTPS response body does not fit into application provided response buffer at location 0x%x "
                "with length: %d",
                pHttpsResponse->pBody,
                pHttpsResponse->pBodyEnd - pHttpsResponse->pBody );
            HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_MESSAGE_TOO_LARGE);
        }
    }

    HTTPS_FUNCTION_CLEANUP_BEGIN();

    /* Set the buffer state to finished for both success or error. This is so that the application can still
       read headers copied even if there was an error. PROCESSING_STATE_FINISHED state is checked in the https parsing 
       callacks to know if we are currently parsing a new response or we are simply parsing to read header values.*/
    pHttpsResponse->bufferProcessingState = PROCESSING_STATE_FINISHED;

    /* Flush the socket of the rest of the data if there is data left from this response. We need to do this
       so that for the next request on this connection, there is not left over response from this request in
       the next response buffer.

       If a continuous stream of data is coming in from the connection, with an unknown end, we may not be able to
       flush the network data. It may sit here forever. A continuous stream should be ingested with the async workflow. 
        
       All network errors are ignore here because network read will have read the data from network buffer despite
       errors. */
    if( pHttpsConnection != NULL )
    {
        flushStatus = _flushHttpsNetworkData( pHttpsConnection, pHttpsResponse );

        if( flushStatus == IOT_HTTPS_PARSING_ERROR)
        {
            IotLogWarn("There an error parsing the network flush data. The network buffer might not be fully flushed. "
                "Error code %d.", 
                flushStatus);
        }
        else if(HTTPS_FAILED(flushStatus))
        {
            IotLogDebug("Received network error when flushing the socket. Error code: %d", flushStatus);
        }

        /* If this is not a persistent connection, the server would have closed it after
           sending a response, but we disconnect anyways. */
        if(reqHandle->isNonPersistent)
        {
            disconnectStatus = IotHttpsClient_Disconnect( pHttpsConnection );
            if( HTTPS_FAILED(disconnectStatus) )
            {
                IotLogWarn("Failed to disconnected from the server with error: %d", status );
                /* The disconnect status is not returned as the server may already be disconnected. */
            }
        }

        /* Let the network thread know that reading from the socket is finished. Whatever context called
           the network receive callback will return from that callback now. */
        IotSemaphore_Post( &( pHttpsConnection->rxFinishSem ) );
        /* Let other threads waiting know the connection is ready to be used. */
        IotSemaphore_Post( &(pHttpsConnection->connSem) );
    }

    /* If there was an error from the parser or other synchronous workflow error NOT from the network, then we want to 
       report this. Parsing errors will close the connection. Otherwise we only report the network error if the parsing 
       failed at the same time. */
    if(HTTPS_FAILED(status) && HTTPS_FAILED(networkStatus)) 
    {
        status = networkStatus;
    }
 
    IOT_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_ReadResponseStatus(IotHttpsResponseHandle_t respHandle, uint16_t *pStatus)
{
    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(respHandle);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pStatus);

    HTTPS_ON_ARG_ERROR_MSG_GOTO_CLEANUP(respHandle->status != 0,
        IOT_HTTPS_NOT_FOUND,
        "The HTTP response status was not found in the HTTP response header buffer.");

    *pStatus = respHandle->status;
    
    HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_ReadHeader(IotHttpsResponseHandle_t respHandle, char *pName, char *pValue, uint32_t len)
{
    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    const char * pHttpParserErrorDescription = NULL;
    IotHttpsResponseBufferState_t savedState = PROCESSING_STATE_NONE;
    size_t numParsed = 0;

    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(respHandle);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pName);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pValue);

    /* Save the current state of the response's buffer processing. */
    savedState = respHandle->bufferProcessingState;

    /* Initialize the header field search parameters. */
    respHandle->pReadHeaderField = pName;
    respHandle->foundHeaderField = false;
    respHandle->bufferProcessingState = PROCESSING_STATE_SEARCHING_HEADER_BUFFER;

    /* Start over the HTTP parser so that it will parser from the beginning of the message. */
    http_parser_init( &( respHandle->httpParserInfo.parser ), HTTP_RESPONSE );
    respHandle->httpParserInfo.parser.data = (void *)(respHandle);

    numParsed = respHandle->httpParserInfo.parseFunc(&(respHandle->httpParserInfo.parser), &_httpParserSettings, (char*)(respHandle->pHeaders), respHandle->pHeadersCur - respHandle->pHeaders);
    IotLogDebug("Parsed %d characters in IotHttpsClient_ReadHeader().", numParsed);
    if( (respHandle->httpParserInfo.parser.http_errno != 0) && 
        ( HTTP_PARSER_ERRNO( &(respHandle->httpParserInfo.parser) ) > HPE_CB_chunk_complete) )
    {
        pHttpParserErrorDescription = http_errno_description( HTTP_PARSER_ERRNO( &(respHandle->httpParserInfo.parser) ) );
        IotLogError("http_parser failed on the http response with error: %s", pHttpParserErrorDescription);
        HTTPS_SET_AND_GOTO_CLEANUP( IOT_HTTPS_PARSING_ERROR);
    }

    if(respHandle->foundHeaderField)
    {
        /* The len of the pValue buffer must account for the NULL terminator. */
        if(respHandle->readHeaderValueLength > (len - 1))
        {
            IotLogError("IotHttpsClient_ReadHeader(): The length of the pValue buffer specified is less than the actual length of the pValue. ");
            HTTPS_SET_AND_GOTO_CLEANUP( IOT_HTTPS_INSUFFICIENT_MEMORY );
        }
        else
        {
            /* strncpy adds a NULL terminator. */
            strncpy(pValue, respHandle->pReadHeaderValue, respHandle->readHeaderValueLength);
        }
    }
    else
    {
        IotLogError("IotHttpsClient_ReadHeader(): The header field %s was not found.", pName);
        HTTPS_SET_AND_GOTO_CLEANUP( IOT_HTTPS_NOT_FOUND );
    }

    HTTPS_FUNCTION_CLEANUP_BEGIN();
    /* Always restore the state back to what it was before entering this function. */
    if( respHandle != NULL )
    {
        respHandle->bufferProcessingState = savedState;
    }
    HTTPS_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_ReadContentLength(IotHttpsResponseHandle_t respHandle, uint32_t *pContentLength)
{   
    HTTPS_FUNCTION_ENTRY(IOT_HTTPS_OK);

    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(respHandle);
    HTTPS_ON_NULL_ARG_GOTO_CLEANUP(pContentLength);

    /* If there is no content-length header or if we were not able to store it in the header buffer this will be
    invalid. */
    if(respHandle->contentLength <= 0)
    {
        IotLogError("The content length not found in the HTTP response header buffer.");
        *pContentLength = 0;
        HTTPS_SET_AND_GOTO_CLEANUP(IOT_HTTPS_NOT_FOUND);
    }

    *pContentLength = respHandle->contentLength;

   HTTPS_FUNCTION_EXIT_NO_CLEANUP();
}

/*-----------------------------------------------------------*/
