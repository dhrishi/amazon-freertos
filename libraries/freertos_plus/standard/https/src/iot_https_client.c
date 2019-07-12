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

/* Error handling include. */
#include "private/iot_error.h"

/* HTTPS Client library private inclues. */
#include "private/iot_https_internal.h"

/*-----------------------------------------------------------*/

/**
 * @brief The length of the end of the header line.
 * 
 * This is the string length of "\r\n". This defined here for use in initializing local string arrays.
 */
#define HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH  ( 2 )

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
 * This is used for writing headers automatically in during the sending of the HTTP request.
 * "Connection: keep-alive\r\n" is written automatically for a persistent connection.
 * "Connection: close\r\n" is written automatically for a closed connection.
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
 * The user buffer is configured in IotHttpsClientRequestInfo_t.reqUserBuffer. This buffer stores the internal context
 * of the request then the request headers right after. The minimum size for the buffer is the total size of the 
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
 * The user buffer is configured in IotHttpsClientRequestInfo_t.respUserBuffer. This buffer stores the internal context
 * of the response and then the response headers right after. This minimum size is calculated for if there are no bytes
 * from the HTTP response headers stored. 
 */
const uint32_t responseUserBufferMinimumSize = sizeof(_httpsResponse_t);

/**
 * @brief Minimum size of the connection user buffer.
 * 
 * The user buffer is configured in IotHttpsConnectionInfo_t.userBuffer. This buffer stores the internal context of the 
 * connection.
 */
const uint32_t connectionUserBufferMinimumSize = sizeof(_httpsConnection_t);

/*-----------------------------------------------------------*/

/**
 * @brief Callback for http-parser to indicate the start of the HTTP response message is reached.
 * 
 * @param[in] pHttpParser - http-parser state structure.
 * 
 * @return 0 to tell http-parser to keep parsing.
 *         1 to tell http-parser that parsing should stop return from http_parser_execute with error HPE_CB_message_begin.
 */
static int _httpParserOnMessageBeginCallback(http_parser * pHttpParser);

/**
 * @brief Callback for http-parser to indicate it found the HTTP response status code.
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
 * @brief Callback for http-parser to indicate it found an HTTP response header field.
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
 * @brief Callback for http-parser to indicate it found an HTTP response header value.
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
 * @brief Callback for http-parser to indicate it reached the end of the headers in the HTTP response messsage.
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
 * @brief Callback for http-parser to indicate it found HTTP response body.
 * 
 * This callback will be invoked multiple times if there response body is of "Transfer-Encoding: chunked". 
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
 * @brief Callback for http-parser to indicate it reached the end of the HTTP response messsage.
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

/* To save code space we do not compile in this code that just prints debugging. */
#if (LIBRARY_LOG_LEVEL == IOT_LOG_DEBUG)
/**
 * @brief Callback for http-parser to indicate it found an HTTP Transfer-Encoding: chunked header.
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
 * @brief Callback for http-parser to indicate it reached the end of an HTTP response messsage "chunk".
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
 * This function is called by the network abstraction whenever data is available for the HTTP library.
 * 
 * @param[in] pNetworkConnection - The network connection with the HTTPS connection, pass by the network stack.
 * @param[in] pReceiveContext - A pointer to the HTTPS Client connection handle for which the packet was received.
 */
static void _networkReceiveCallback( void* pNetworkConnection, void* pReceiveContext );

/**
 * @brief Connects to HTTPS server and initializes the connection context.
 * 
 * @param[out] pConnHandle - Handle returned representing the open connection.
 * @param[in] pConnInfo - The connection configuration.
 * 
 * @return #IOT_HTTPS_OK if the connection was successful and so was initializing the context.
 *         #IOT_HTTPS_CONNECTION_ERROR if the connection failed.
 *         #IOT_HTTPS_INTERNAL_ERROR if the context failed to initialize.
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
 * @param[in] pValue - The buffer to value string.
 * @param[in] valueLen - The length of the header value string.
 * 
 * @return #IOT_HTTPS_OK if the header was added to the request successfully.
 *         #IOT_HTTPS_INSUFFICIENT_MEMORY if there was not room in the IotHttpsRequestHandle_t->pHeaders.
 */
static IotHttpsReturnCode_t _addHeader(_httpsRequest_t * pHttpsRequest, const char * pName, const char * pValue, uint32_t valueLen );

/**
 * @brief Send data on the network.
 * 
 * @param[in] pHttpsConnection - HTTP connection context.
 * @param[in] pBuf - Buffer of data to send.
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
 * @param[in] pBuf - Buffer of data to receive into.
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
 * @param[in] pHeadersBuf - Buffer of the request headers to send. This buffer must contain HTTP headers lines without the 
 *      indicator for the the end of the HTTP headers.
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
 * @param[in] pBuf - The buffer of data to parse.
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
 * @param[in] pBuf - Pointer to the buffer to receive the HTTP response into.
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
                                                  uint8_t** pBuf,
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
 * pHttpsResponse->pHeaders buffer, then we need to flush the network buffer.
 * 
 * If the application configured the body buffer as null in IotHttpsResponseInfo_t.syncInfo.respData and the server 
 * sends body in the response, but it exceeds the size of  pHttpsResponse->pHeaders buffer, then we need to flush the 
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
 * @return #IOT_HTTPS_OK if we received successfully flushed the network data.
 *         #IOT_HTTPS_PARSING_ERROR if there was an error with parsing the data.
 *         #IOT_HTTPS_NETWORK_ERROR if there was an error receiving the data on the network.
 *         #IOT_HTTPS_TIMEOUT_ERROR if we timedout trying to receive data from the network.
 */
static IotHttpsReturnCode_t _flushHttpsNetworkData( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse );

/**
 * @brief Task pool job routine to send the HTTP request within the pUserContext.
 * 
 * @param[in] pTaskPool Pointer to the system task pool.
 * @param[in] pJob Pointer the to the HTTP request sending job.
 * @param[in] pContext Pointer to an HTTP request, passed as an opaque context.
 */
static void _sendHttpsRequest( IotTaskPool_t pTaskPool, IotTaskPoolJob_t pJob, void * pUserContext );

/**
 * @brief Implicitly connect if the pConnHandle is NULL or the current connection in pConnHandle is disconnected.
 * 
 * @param[in] pConnHandle - Handle from an HTTPS connection. If points to NULL then an implicit connection will be made.
 * @param[in] pConnInfo - Connection configuration information.
 * 
 * @return  #IOT_HTTPS_OK - if the request was sent and the response was received successfully.
 *          #IOT_HTTPS_CONNECTION_ERROR if the connection failed.
 */
static IotHttpsReturnCode_t _implicitlyConnect(IotHttpsConnectionHandle_t *pConnHandle, IotHttpsConnectionInfo_t* pConnInfo);


/**
 * @brief Receive the HTTPS body specific to an asynchronous type of response.
 * 
 * @param[in] pHttpsResponse - HTTP response context.
 * @param[out] pNetworkStatus - The network status of receiving the body synchronously.
 * 
 * @return  #IOT_HTTPS_OK - If the the response body was received with no issues. 
 *          #IOT_HTTPS_ASYNC_CANCELLED - If the request was cancelled by the Application
 *          #IOT_HTTPS_PARSING_ERROR - If there was an issue parsing the HTTP response body.
 */
static IotHttpsReturnCode_t _receiveHttpsBodyAsync(_httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t* pNetworkStatus);

/**
 * @brief Receive the HTTPS body specific to a synchronous type of response.
 * 
 * @param[in] pHttpsResponse - HTTP response context.
 * @param[out] pNetworkStatus - The network status of receiving the body synchronously.
 * 
 * @return  #IOT_HTTPS_OK - If the the response body was received with no issues. 
 *          #IOT_HTTPS_MESSAGE_TOO_LARGE - If the body from the network is too large to fit into the configured body buffer.
 *          #IOT_HTTPS_PARSING_ERROR - If there was an issue parsing the HTTP response body.
 */
static IotHttpsReturnCode_t _receiveHttpsBodySync(_httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t* pNetworkStatus);

/**
 * @brief Schedule the task to send the the HTTP request.
 * 
 * @param[in] pHttpsResponse - HTTP response context.
 * 
 * @return  #IOT_HTTPS_OK - If the task to send the HTTP request was successfully scheduled.
 *          #IOT_HTTPS_INTERNAL_ERROR - If a taskpool job could not be created.
 *          #IOT_HTTPS_ASYNC_SCHEDULING_ERROR - If there was an error scheduling the job.
 */
IotHttpsReturnCode_t _scheduleHttpsRequestSend(_httpsRequest_t* pHttpsRequest);

/**
 * @brief Add the request to the connection's request queue.
 * 
 * This will schdule a task if the request is first and only request in the queue.
 * 
 * @return  #IOT_HTTPS_OK - If the request was successfully added to the connection's request queue.
 *          #IOT_HTTPS_INTERNAL_ERROR - If a taskpool job could not be created.
 *          #IOT_HTTPS_ASYNC_SCHEDULING_ERROR - If there was an error scheduling the job.
 */
IotHttpsReturnCode_t _addRequestToConnectionReqQ(_httpsRequest_t* pHttpsRequest);

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
    int retVal = 0;
    IotLogDebug("Parser: Start of HTTPS Response message.");
    
    _httpsResponse_t *pHttpsResponse = (_httpsResponse_t*)(pHttpParser->data);
    /* Set the state of the parser. The headers are at the start of the message always. */
    pHttpsResponse->parserState = PARSER_STATE_IN_HEADERS;
    return retVal;
}

/*-----------------------------------------------------------*/

static int _httpParserOnStatusCallback(http_parser * pHttpParser, const char * pLoc, size_t length)
{
    _httpsResponse_t *pHttpsResponse = (_httpsResponse_t*)(pHttpParser->data);
    IotLogDebug("Parser: Status %.*s retrieved from HTTPS response.", length, pLoc);

    /* Save the status code so it can be retrieved with IotHttpsClient_ReadResponseStatus(). */
    pHttpsResponse->status = (uint16_t)(pHttpParser->status_code);
    /* If we are parsing the network data received in the header buffer then we can increment 
      pHttpsResponse->pHeadersCur. The status line in the response is part of the data stored in
      pHttpsResponse->pHeaders. */
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
    IotLogDebug("Parser: End of the headers reached.");

    int retVal = 0;
    _httpsResponse_t * pHttpsResponse = (_httpsResponse_t *)(pHttpParser->data);
    pHttpsResponse->parserState = PARSER_STATE_HEADERS_COMPLETE;

    /* If the IotHttpsClient_ReadHeader() was called, we return after finishing looking through all of the headers. 
       Returning a non-zero value exits the http parsing. */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_SEARCHING_HEADER_BUFFER)
    {
        retVal = 1;
    }
    
    /* When in this callback the pHeaderCur pointer is at the first "\r" in the last header line. HTTP/1.1
    headers end with another "\r\n" at the end of the last line. This means we must increment
    the headerCur pointer to the length of "\r\n\r\n". */
    if(pHttpsResponse->bufferProcessingState == PROCESSING_STATE_FILLING_HEADER_BUFFER)
    {
        pHttpsResponse->pHeadersCur += strlen("\r\n\r\n");
    }

    /* content_length will be zero if no Content-Length header found by the parser. */
    pHttpsResponse->contentLength = (uint32_t)(pHttpParser->content_length);
    IotLogDebug("Parser: Content-Length found is %d.", pHttpsResponse->contentLength);

    if(pHttpsResponse->bufferProcessingState < PROCESSING_STATE_FINISHED)
    {
        /* For a HEAD method, there is no body expected in the response, so we return 1 to skip body parsing.
        Also if it was configured in a synchronous response to ignore the HTTPS response body then also stop the body
        parsing. */
        if( ( pHttpsResponse->method == IOT_HTTPS_METHOD_HEAD ) ||
            ( ( pHttpsResponse->isAsync == false ) && ( pHttpsResponse->pBody == NULL ) ) )
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

    if((pHttpsResponse->bufferProcessingState == PROCESSING_STATE_FILLING_HEADER_BUFFER) && (pHttpsResponse->isAsync))
    {
        /* For an asynchronous response, the buffer to store the body will be available after the headers 
         * are read first. We may receive part of the body in the header buffer. We will want to leave this here
         * and copy it over when the body buffer is available in the _readReadyCallback().
         */
        if( pHttpsResponse->pBodyStartInHeaderBuf == NULL )
        {
            pHttpsResponse->pBodyStartInHeaderBuf = ( uint8_t* )( pLoc );
        }
        pHttpsResponse->bodyLengthInHeaderBuf += (uint32_t)length;

    }
    else if(pHttpsResponse->bufferProcessingState < PROCESSING_STATE_FINISHED)
    {
        /* Only copy the data if the current location is not the bodyCur. Also only copy if the length does not
           exceed the body buffer. This might happen, only in the synchronous workflow, if the header buffer is larger 
           than the body buffer and receives entity body larger than the body bufffer. */
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

    /* When this callback is reached the end of the HTTP message is indicated. We return a 1 here so that we can stop 
       parsing. When we support pipelined requests, we can check if the next response tailgated/piggybacked onto this
       buffer by checking if there is a status code available in pBodyCur + 1 (PROCESSING_STATE_FILLING_BODY_BUFFER) or 
       pHeaderCur + 1 ( for PROCESSING_STATE_FILLING_HEADER_BUFFER). "*/
    return 1;
}

/*-----------------------------------------------------------*/

/* To save code space we do not compile in this code that just prints debugging. */
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

static IotHttpsReturnCode_t _receiveHttpsBodyAsync(_httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t* pNetworkStatus)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    _httpsRequest_t* pHttpsRequest = pHttpsResponse->pHttpsRequest;

    if(pHttpsRequest->pCallbacks->readReadyCallback)
    {
        /* If there is still more body that we have not passed back to the user, then we need to call the callback again. */
        do {
            pHttpsRequest->pCallbacks->readReadyCallback(pHttpsRequest->pUserPrivData, 
                pHttpsResponse, 
                pHttpsResponse->bodyRxStatus, 
                pHttpsResponse->status);
            if(pHttpsResponse->cancelled == true)
            {
                IotLogDebug("Cancelled HTTP request %d.", pHttpsResponse->pHttpsRequest);
                status = IOT_HTTPS_ASYNC_CANCELLED;
                break;
            }
        } while((pHttpsResponse->parserState < PARSER_STATE_BODY_COMPLETE) && (pHttpsResponse->bodyRxStatus == IOT_HTTPS_OK));

        if(pHttpsResponse->bodyRxStatus != IOT_HTTPS_OK)
        {
            IotLogError("Error receiving the HTTP response body for request %d. Error code: %d",
                pHttpsResponse->pHttpsRequest,
                pHttpsResponse->bodyRxStatus);
        }

        if(pHttpsResponse->parserState < PARSER_STATE_BODY_COMPLETE)
        {
            IotLogDebug("Did not receive all of the HTTP response body for request %d.", 
                pHttpsResponse->pHttpsRequest);
        }
    }

    *pNetworkStatus = pHttpsResponse->bodyRxStatus;
    return status;
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _receiveHttpsBodySync(_httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t* pNetworkStatus)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    _httpsConnection_t* pHttpsConnection = pHttpsResponse->pHttpsConnection;

    /* The header buffer is now filled or the end of the headers has been reached already. If part of the response
        body was read from the network into the header buffer, then it was already copied to the body buffer in the 
        _httpParserOnBodyCallback(). */
    if(pHttpsResponse->pBody != NULL)
    {
        /* If there is room left in the body buffer, then try to receive more. */
        if( (pHttpsResponse->pBodyEnd - pHttpsResponse->pBodyCur) > 0 )
        {
            status = _receiveHttpsBody( pHttpsConnection,
                pHttpsResponse,
                pNetworkStatus );
            if( status != IOT_HTTPS_OK )
            {
                IotLogError( "Error receiving the HTTPS response body for response %d. Error code: %d.", 
                    pHttpsResponse,
                    status, 
                    *pNetworkStatus );
            }

            if((*pNetworkStatus != IOT_HTTPS_OK) && (*pNetworkStatus != IOT_HTTPS_TIMEOUT_ERROR))
            {
                IotLogError( "Network error receiving HTTPS body. Error code: %d.", *pNetworkStatus);
            }
        }
        else
        {
            IotLogDebug("Received the maximum amount of HTTP body when filling the header buffer for response %d.",
                pHttpsResponse);
        }

        /* If we don't reach the end of the HTTPS body in the parser, then we only received part of the body.
            The rest of body will be on the socket. */
        if( (status == IOT_HTTPS_OK) && (pHttpsResponse->parserState < PARSER_STATE_BODY_COMPLETE ))
        {
            IotLogError( "HTTPS response body does not fit into application provided response buffer at location 0x%x with length: %d",
                pHttpsResponse->pBody,
                pHttpsResponse->pBodyEnd - pHttpsResponse->pBody );
            status = IOT_HTTPS_MESSAGE_TOO_LARGE;
        }
    }
    else
    {
        IotLogDebug("No response body was configure for response %d.", pHttpsResponse);
    }

    return status;
}

/*-----------------------------------------------------------*/

static void _networkReceiveCallback( void* pNetworkConnection, void* pReceiveContext )
{
    IOT_FUNCTION_ENTRY( IotHttpsReturnCode_t, IOT_HTTPS_OK );

    IotHttpsReturnCode_t networkStatus = IOT_HTTPS_OK;
    IotHttpsReturnCode_t flushStatus = IOT_HTTPS_OK;
    _httpsConnection_t* pHttpsConnection = ( _httpsConnection_t* )pReceiveContext;
    _httpsResponse_t* pCurrentHttpsResponse = NULL;
    _httpsResponse_t* pNextHttpsResponse = NULL;
    _httpsRequest_t* pCurrentHttpsRequest = NULL;
    _httpsRequest_t* pNextHttpsRequest = NULL;
    IotLink_t * pQItem = NULL;
    bool fatalDisconnect = false; 

    /* The network connection is already in the connection context. */
    ( void )pNetworkConnection;

    /* Dequeue response from the response queue. */
    /* For now the responses are in a queue. When pipelining is supported, the code will need to be updated to copy
       responses tailgating in buffers to other buffers. */
    IotMutex_Lock( &( pHttpsConnection->respQMutex ) );
    pQItem = IotDeQueue_DequeueHead( &(pHttpsConnection->respQ));
    IotMutex_Unlock( &(pHttpsConnection->respQMutex) );

    /* If the receive callback is invoked and there is no response expected, then this a violation of the HTTP/1.1
       protocol. */
    if( pQItem == NULL )
    {
        IotLogError("Received data on the network, when no response was expected...");
        fatalDisconnect = true;
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_NETWORK_ERROR);
    }

    /* Set the current HTTP response context to use. */
    pCurrentHttpsResponse = IotLink_Container(_httpsResponse_t, pQItem, link);

    /* Set the current HTTP request associated with this response. */
    pCurrentHttpsRequest = pCurrentHttpsResponse->pHttpsRequest;

    /* If the receive callback has invoked, but the request associated with this response has not finished sending 
       to the server, then this is a violation of the HTTP/1.1 protocol.  */
    if(pCurrentHttpsRequest->reqFinishedSending == false)
    {
        IotLogError("Received response data on the network when the request was not finished sending. This is unexpected.");
        fatalDisconnect = true;
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_NETWORK_ERROR);
    }

    /* If the current response was cancelled, then we need to not bother receiving the headers and body. */
    if(pCurrentHttpsResponse->cancelled)
    {
        IotLogDebug("Request ID: %d was canceled.");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_ASYNC_CANCELLED);
    }

    /* Reset the http-parser state to an initial state. This is done so that a new response can be parsed from the 
       beginning. */
    pCurrentHttpsResponse->httpParserInfo.parser.data = (void *)(pCurrentHttpsResponse);
    pCurrentHttpsResponse->parserState = PARSER_STATE_NONE;
    pCurrentHttpsResponse->bufferProcessingState = PROCESSING_STATE_FILLING_HEADER_BUFFER;

    /* Receive the response from the network. */
    /* Receive the headers first. */
    status = _receiveHttpsHeaders(pHttpsConnection, pCurrentHttpsResponse, &networkStatus);
    if( status != IOT_HTTPS_OK )
    {
        IotLogError("Error receiving the HTTPS headers with error code: %d", status);
        /* If there were errors parsing then we may have received rouge data from a rouge server and should disconnect. */
        fatalDisconnect = true;
        IOT_GOTO_CLEANUP();
    }

    /* If the network status is a timeout error, then that does not mean anything went wrong. All other network
       errors may be transient and we simply want to report it and move on.*/
    if((networkStatus != IOT_HTTPS_OK) && (networkStatus != IOT_HTTPS_TIMEOUT_ERROR))
    {
        IotLogError("Network error when receiving HTTPS headers. Error code: %d", networkStatus);
    }

    if( pCurrentHttpsResponse->parserState < PARSER_STATE_HEADERS_COMPLETE )
    {
        IotLogDebug( "Headers received on the network did not all fit into the configured header buffer for request %d."
            " The length of the headers buffer is: %d",
            pCurrentHttpsRequest,
            pCurrentHttpsResponse->pHeadersEnd - pCurrentHttpsResponse->pHeaders );
        /* It is not error if the headers did not all fit into the buffer. */
    }

    /* Receive the body. */
    if(pCurrentHttpsResponse->isAsync)
    {
        status = _receiveHttpsBodyAsync(pCurrentHttpsResponse, &networkStatus);
    }
    else
    {
        /* Otherwise receive synchronously. */
        status = _receiveHttpsBodySync(pCurrentHttpsResponse, &networkStatus);
    }

    /* If the network status is a timeout error, then that does not mean anything went wrong. All other network
       errors may be transient and we simply want to pass that up to the application.*/
    if((networkStatus != IOT_HTTPS_OK) && (networkStatus != IOT_HTTPS_TIMEOUT_ERROR))
    {
        IotLogError("Network error receiving HTTPS body synchronously. Error code %d", networkStatus);
    }

    if(status != IOT_HTTPS_OK)
    {
        if((status == IOT_HTTPS_ASYNC_CANCELLED))
        {
            /* The user could have cancelled which is not really an error, but we still want to stop. */
            IotLogDebug("User cancelled during the async readReadyCallback() for request %d.", 
                pCurrentHttpsRequest);
            
        }
        else if((status == IOT_HTTPS_PARSING_ERROR))
        {
            /* There was an error parsing the HTTPS response body. This may be an indication of a server that does
                not adhere to protocol correctly. We should disconnect. */
            IotLogError("Failed to parse the HTTPS body for request %d, Error code: %d.", 
                pCurrentHttpsRequest,
                status);
            fatalDisconnect = true;
        }
        else
        {
            IotLogError("Failed to retrive the HTTPS body for request %d. Error code: %d", networkStatus);
        }
        IOT_GOTO_CLEANUP();
    }

    IOT_FUNCTION_CLEANUP_BEGIN();

    pCurrentHttpsResponse->syncStatus = status;
    
    /* If there was a network error, then report this to the application. A timeout is is not always an error since 
       we ask  */
    if((networkStatus != IOT_HTTPS_OK) && (networkStatus != IOT_HTTPS_TIMEOUT_ERROR))
    {
        if(pCurrentHttpsResponse->isAsync && pCurrentHttpsRequest->pCallbacks->errorCallback)
        {
            pCurrentHttpsRequest->pCallbacks->errorCallback(pCurrentHttpsRequest->pUserPrivData, pCurrentHttpsRequest, networkStatus);
        }
    }

    /* If there was an error from the parser or other synchronous workflow error NOT from the network, then we want to report this.
       Parsing errors will close the connection. Otherwise we only report the network error if the parsing failed at the same time. */
    if(status != IOT_HTTPS_OK) 
    {
        if(pCurrentHttpsResponse->isAsync && pCurrentHttpsRequest->pCallbacks->errorCallback)
        {
            pCurrentHttpsRequest->pCallbacks->errorCallback(pCurrentHttpsRequest->pUserPrivData, pCurrentHttpsRequest, status);
        }

        if(networkStatus != IOT_HTTPS_OK)
        {
            pCurrentHttpsResponse->syncStatus = networkStatus;
        }
    }   

    /* If this is not a persistent connection, the server would have closed it after 
       sending a response, but we disconnect anyways. If we are disconnecting there is is no point in wasting time 
       flushing the network. If the network is being disconnected we also do not schedule any pending requests. */
    if( fatalDisconnect || pCurrentHttpsRequest->isNonPersistent )
    {
        if(pCurrentHttpsResponse->isAsync && pCurrentHttpsRequest->pCallbacks->errorCallback)
        {
            pCurrentHttpsRequest->pCallbacks->errorCallback(pCurrentHttpsRequest->pUserPrivData, pCurrentHttpsRequest, networkStatus);
        }
        else
        {
            pCurrentHttpsResponse->syncStatus = networkStatus;
        }

        status = IotHttpsClient_Disconnect(pHttpsConnection);
        if( status != IOT_HTTPS_OK )
        {
            IotLogWarn("Failed to disconnected from the server with return code: %d", status);
        }

        if((pCurrentHttpsResponse != NULL) && pCurrentHttpsResponse->isAsync && pCurrentHttpsRequest->pCallbacks->connectionClosedCallback)
        {
            pCurrentHttpsRequest->pCallbacks->connectionClosedCallback(pCurrentHttpsRequest->pUserPrivData, pHttpsConnection, status);
        }

        /* If we disconnect, we do not process anymore requests. */
    }
    else
    {
        /* Set the processing state of the buffer to finished for completeness. This is also to prevent the parsing of the flush
           data from incrementing any pointer in the HTTP response context. */
        pCurrentHttpsResponse->bufferProcessingState = PROCESSING_STATE_FINISHED;
        
        /* Flush the socket of the rest of the data if there is data left from this response. We need to do this
           so that for the next request on this connection, there is not left over response from this request in
           the next response buffer.

           If a continuous stream of data is coming in from the connection, with an unknown end, we may not be able to
           flush the network data. It may sit here forever. A continuous stream should be ingested with the async workflow. 
        
           All network errors are ignore here because network read will have read the data from network buffer despite
           errors. */
        flushStatus = _flushHttpsNetworkData( pHttpsConnection, pCurrentHttpsResponse );
        if( flushStatus == IOT_HTTPS_PARSING_ERROR)
        {
            IotLogWarn("There an error parsing the network flush data. The network buffer might not be fully flushed.");
        }
        else if( flushStatus != IOT_HTTPS_OK )
        {
            IotLogDebug("Network error when flushing the https network data: %d", flushStatus);
        }
        
        IotMutex_Lock(&(pHttpsConnection->reqQMutex));
        /* Now that the current request/response pair is finished, we dequeue the current request from the queue. */
        pQItem = IotDeQueue_DequeueHead( &(pHttpsConnection->reqQ));
        /* Get the next request to process. */
        pQItem = IotDeQueue_PeekHead( &(pHttpsConnection->reqQ));
        IotMutex_Unlock(&(pHttpsConnection->reqQMutex));

        /* If there is a next request to process, then create a taskpool job to send the request. */
        if(pQItem != NULL)
        {
            /* Set this next request to send. */
            pNextHttpsRequest = IotLink_Container(_httpsRequest_t, pQItem, link);
            /* Set the next response to receive. */
            pNextHttpsResponse = pNextHttpsRequest->pHttpsResponse;

            IotLogDebug("Request %d is next in the queue. Now scheduling a task to send the request.", pNextHttpsRequest);
            status = _scheduleHttpsRequestSend(pNextHttpsRequest);
            /* If there was an error with scheduling the new task, then report it. */
            if(status != IOT_HTTPS_OK)
            {
                IotLogError("Error scheduling HTTPS request %d. Error code: %d", pNextHttpsRequest, status);
                if(pNextHttpsResponse->isAsync && pNextHttpsRequest->pCallbacks->errorCallback)
                {
                    pNextHttpsRequest->pCallbacks->errorCallback(pNextHttpsRequest->pUserPrivData, pNextHttpsRequest, status);
                }
                else
                {
                    pNextHttpsResponse->syncStatus = status;
                }
            }
        }
        else
        {
            IotLogDebug("Network receive callback found the request queue empty. A network send task was not scheduled.");
        }
    }

    /* Signal to a synchronous reponse that the response is complete. */
    if( pCurrentHttpsResponse->isAsync && pCurrentHttpsRequest->pCallbacks->responseCompleteCallback )
    {
        pCurrentHttpsRequest->pCallbacks->responseCompleteCallback( pCurrentHttpsRequest->pUserPrivData, pCurrentHttpsResponse, networkStatus, pCurrentHttpsResponse->status );
    }

    /* For a synchronous request release the semaphore. */
    if(pCurrentHttpsResponse->isAsync == false)
    {
        IotSemaphore_Post(&(pCurrentHttpsResponse->respFinishedSem));
    }
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_Init( void )
{
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
/* To save code space we do not compile in code that just prints debugging. */
#if (LIBRARY_LOG_LEVEL == IOT_LOG_DEBUG)
    _httpParserSettings.on_chunk_header = _httpParserOnChunkHeaderCallback;
    _httpParserSettings.on_chunk_complete = _httpParserOnChunkCompleteCallback;
#endif

    return IOT_HTTPS_OK;
}

/*-----------------------------------------------------------*/

void IotHttpsClient_Deinit( void )
{
    /* The library has not taken any resources that need freeing. This implementation is here for formality.*/
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _createHttpsConnection(IotHttpsConnectionHandle_t * pConnHandle, IotHttpsConnectionInfo_t *pConnInfo)
{
    IOT_FUNCTION_ENTRY(IotHttpsReturnCode_t, IOT_HTTPS_OK);

    IotNetworkError_t networkStatus = IOT_NETWORK_SUCCESS;
    /* The maxmimum string length of the ALPN protocols is configured in IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH. 
       This is +1 for the unfortunate NULL terminator needed by IotNetworkCredentials_t.pAlpnProtos. */
    char pAlpnProtos[IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH + 1] = { 0 };
    /* The maximum string length of the Server host name is configured in IOT_HTTPS_MAX_HOST_NAME_LENGTH. 
       This is +1 for the unfortunate NULL terminator needed by IotNetworkServerInfo_t.pHostName. */
    char pHostName[IOT_HTTPS_MAX_HOST_NAME_LENGTH + 1] = { 0 };
    bool reqQMutexCreated = false;
    bool respQMutexCreated = false;
    IotNetworkServerInfo_t networkServerInfo = { 0 };
    IotNetworkCredentials_t networkCredentials = { 0 };
    _httpsConnection_t *pHttpsConnection = NULL;

    /* Make sure the connection context can fit in the user buffer. */
    if(pConnInfo->userBuffer.bufferLen < connectionUserBufferMinimumSize)
    {
        IotLogError("Buffer size is too small to initialize the connection context. User buffer size: %d, required minimum size; %d.", 
            (*pConnInfo).userBuffer.bufferLen, 
            connectionUserBufferMinimumSize);
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INSUFFICIENT_MEMORY);
    }

    /* Set the internal connection context to the start of the user buffer. */
    if(pConnInfo->userBuffer.pBuffer == NULL)
    {
        IotLogError("IotHttpsConnectionInfo_t.userBuffer.pBuffer was NULL.");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INVALID_PARAMETER);
        
    }
    pHttpsConnection = (_httpsConnection_t *)(pConnInfo->userBuffer.pBuffer);
    
    /* Set to disconnected initially. */
    pHttpsConnection->isConnected = false;

    /* Initialize the queue of responses and requests. */
    IotDeQueue_Create( &(pHttpsConnection->reqQ) );
    IotDeQueue_Create( &(pHttpsConnection->respQ) );

    /* This timeout is used to wait for a response on the connection. */
    if( pConnInfo->timeout == 0 )
    {
        pHttpsConnection->timeout = IOT_HTTPS_RESPONSE_WAIT_MS;
    }
    else
    {
        pHttpsConnection->timeout = pConnInfo->timeout;
    }

    if( pConnInfo->pNetworkInterface == NULL)
    {
        IotLogError("pNetworkInterface in pConnInfo is NULL.");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INVALID_PARAMETER);
    }
    /* pNetworkInterface contains the connect, disconnect, send, and receive over the network functions. */
    pHttpsConnection->pNetworkInterface = pConnInfo->pNetworkInterface;
    
    /* IotNetworkServerInfo_t should take in the length of the host name instead of requiring a NULL terminator. */
    if((pConnInfo->pAddress == NULL) || (pConnInfo->addressLen == 0))
    {
        IotLogError("IotHttpsConnectionInfo_t.pAddress is NULL or not specified.");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INVALID_PARAMETER);
    }
    if(pConnInfo->addressLen > IOT_HTTPS_MAX_HOST_NAME_LENGTH)
    {
        IotLogError("IotHttpsConnectionInfo_t.addressLen has a host name length %d that exceeds maximum length %d.",
            pConnInfo->addressLen,
            IOT_HTTPS_MAX_HOST_NAME_LENGTH);
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INVALID_PARAMETER);
    }
    memcpy( pHostName, pConnInfo->pAddress, pConnInfo->addressLen );
    pHostName[pConnInfo->addressLen] = '\0';

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
            /* IotNetworkCredentials_t should take in a length for the alpn protocols string instead of requiring a 
            NULL terminator. */
            if( pConnInfo->alpnProtocolsLen > IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH )
            {
                IotLogError( "IotHttpsConnectionInfo_t.alpnProtocolsLen of %d exceeds the configured maximum protocol length %d. See IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH for more information.",
                    pConnInfo->alpnProtocolsLen,
                    IOT_HTTPS_MAX_ALPN_PROTOCOLS_LENGTH );
                IOT_SET_AND_GOTO_CLEANUP( IOT_HTTPS_INVALID_PARAMETER );
            }
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

    /* Check to see if the network connection succeeded. If we did not succeed then the connHandle
       will be NULL and we return an error. */
    if( networkStatus != IOT_NETWORK_SUCCESS )
    {
        IotLogError( "Failed to connect to the server at %.*s on port %d with error: %d",
            pConnInfo->addressLen,
            pConnInfo->pAddress,
            pConnInfo->port,
            networkStatus );
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_CONNECTION_ERROR);
    }
    else
    {
        /* The connection succeeded so this a connected context. */
        pHttpsConnection->isConnected = true;
    }

    /* The receive callback tells the task context handling the request/response that the network
       is ready to read from. */
    networkStatus = pHttpsConnection->pNetworkInterface->setReceiveCallback( pHttpsConnection->pNetworkConnection,
        _networkReceiveCallback,
        pHttpsConnection );
    if( networkStatus != IOT_NETWORK_SUCCESS )
    {
        IotLogError( "Failed to connect to set the HTTPS receive callback. " );
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    /* Connection was successful, so create synchronization primitives. */

    /* Create the mutex protecting operations the queue of requests waiting to be serviced in this connection. */
    reqQMutexCreated = IotMutex_Create( &(pHttpsConnection->reqQMutex), false );
    if(!reqQMutexCreated)
    {
        IotLogError("Failed to create an internal mutex.");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    respQMutexCreated = IotMutex_Create( &(pHttpsConnection->respQMutex), false );
    if(!respQMutexCreated)
    {
        IotLogError("Failed to create an internal mutex.");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    /* Return the new connection information. */
    *pConnHandle = pHttpsConnection;

    IOT_FUNCTION_CLEANUP_BEGIN();

    /* If we failed anywhere in the connection process, then destroy the semaphores created. */
    if(status != IOT_HTTPS_OK)
    {
        /* If there was a connect was successful, disconnect from the network.  */
        if(( pHttpsConnection != NULL) && (pHttpsConnection->isConnected))
        {   
            _networkDisconnect( pHttpsConnection );
            _networkDestroy( pHttpsConnection );
        }

        if(reqQMutexCreated)
        {
            IotMutex_Destroy(&(pHttpsConnection->reqQMutex));
        }

        if(respQMutexCreated)
        {
            IotMutex_Destroy(&(pHttpsConnection->respQMutex));
        }

        /* Set the connection handle as NULL if everything failed. */
        *pConnHandle = NULL;
    }

    IOT_FUNCTION_CLEANUP_END();
}

/* --------------------------------------------------------- */

IotHttpsReturnCode_t IotHttpsClient_Connect(IotHttpsConnectionHandle_t * pConnHandle, IotHttpsConnectionInfo_t *pConnInfo)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    /* Check for NULL parameters in a public API. */
    if((pConnHandle == NULL) || (pConnInfo == NULL))
    {
        IotLogError("Null parameters passed into IotHttpsClient_Connect");
        status = IOT_HTTPS_INVALID_PARAMETER;
    }

    /* If a valid connection handle is passed in. */
    if((status == IOT_HTTPS_OK) && (*pConnHandle != NULL))
    {
        /* If the handle in a connected state, then we want to disconnect before reconnecting. The ONLY way to put the 
           handle is a disconnect state is to call IotHttpsClient_Disconnect(). */
        if((*pConnHandle)->isConnected)
        {
            status = IotHttpsClient_Disconnect(*pConnHandle);
            if(status != IOT_HTTPS_OK)
            {
                IotLogError("Error disconnecting a connected *pConnHandle passed to IotHttpsClient_Connect().Error code %d", status);
                *pConnHandle = NULL;
            }
        }
    }

    /* Connect to the server now. Initialize all resources needed for the connection context as well here. */
    if(status == IOT_HTTPS_OK)
    {
        status = _createHttpsConnection(pConnHandle, pConnInfo);
        if(status != IOT_HTTPS_OK)
        {
            IotLogError("Error in IotHttpsClient_Connect(). Error code %d.", status);
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

static void _networkDisconnect(_httpsConnection_t* pHttpsConnection)
{
    IotNetworkError_t networkStatus = IOT_NETWORK_SUCCESS;
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

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
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    networkStatus = pHttpsConnection->pNetworkInterface->destroy( pHttpsConnection->pNetworkConnection );
    if ( networkStatus != IOT_NETWORK_SUCCESS )
    {
        IotLogWarn("Failed to shutdown the socket with error code: %d", networkStatus );
    }
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_Disconnect(IotHttpsConnectionHandle_t connHandle)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    IotHttpsReturnCode_t networkStatus = IOT_HTTPS_OK;
    _httpsRequest_t* pHttpsRequest = NULL;
    IotLink_t* pItem = NULL;

    if( connHandle == NULL )
    {
        IotLogError("NULL parameter passed into IotHttpsClient_Disconnect().");
        status = IOT_HTTPS_INVALID_PARAMETER;
    }

    if( status == IOT_HTTPS_OK)
    {
        /* Mark the network as disconnected whether the disconnect passes or not. */
        connHandle->isConnected = false;
        
        /* Disconnect from the network. */
        _networkDisconnect(connHandle);

        /* If there is a request in the connection's request queue and it has not finished sending, then we cannot
           destroy the connection until it finishes. */
        /* Still deciding if it is better to wait on a request in process semaphore or return right away if the 
           request is in process. */
        IotMutex_Lock(&(connHandle->reqQMutex));
        pItem = IotDeQueue_PeekHead(&(connHandle->reqQ));
        if(pItem != NULL)
        {
            pHttpsRequest = IotLink_Container(_httpsRequest_t, pItem, link);
            if(pHttpsRequest->reqFinishedSending == false)
            {
                IotLogError("Connection is in use. Disconnected, but cannot destroy the connection.");
                status = IOT_HTTPS_BUSY;
            }
        }
        IotMutex_Unlock(&(connHandle->reqQMutex));

        /* Delete all of the pending requests and responses on the connection. */
        IotDeQueue_RemoveAll(&(connHandle->reqQ), NULL, 0);
        IotDeQueue_RemoveAll(&(connHandle->reqQ), NULL, 0);
    }

    if(status == IOT_HTTPS_OK)
    {

        /* Destroy the network connection (cleaning up network socket resources). */
        _networkDestroy(connHandle);

        /* Destroy the mutexes protecting the request queue and the response queue. */
        IotMutex_Destroy(&(connHandle->reqQMutex));
        IotMutex_Destroy(&(connHandle->respQMutex));
    }

    return status;
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _addHeader(_httpsRequest_t * pHttpsRequest, const char * pName, const char * pValue, uint32_t valueLen )
{
    int nameLen = strlen( pName ) ;
    int headerFieldSeparatorLen = strlen(HTTPS_HEADER_FIELD_SEPARATOR);
    uint32_t additionalLength = nameLen + headerFieldSeparatorLen + valueLen + HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;
    uint32_t possibleLastHeaderAdditionalLength = HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    /* Check if the additional length needed for another header does not exceed the header buffer. */
    if( (additionalLength + possibleLastHeaderAdditionalLength + pHttpsRequest->pHeadersCur) > (pHttpsRequest->pHeadersEnd) )
    {
        IotLogError( "There is %d space left in the header buffer, but we want to add %d more of header.",
            pHttpsRequest->pHeadersEnd - pHttpsRequest->pHeadersCur,
            additionalLength + possibleLastHeaderAdditionalLength );
        status = IOT_HTTPS_INSUFFICIENT_MEMORY;
    }

    if(status == IOT_HTTPS_OK)
    {
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
    }

    return status;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_InitializeRequest(IotHttpsRequestHandle_t * pReqHandle, IotHttpsRequestInfo_t *pReqInfo)
{
    _httpsRequest_t * pHttpsRequest = NULL;
    size_t additionalLength = 0;
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    int spaceLen = 1;
    char* pSpace = " ";
    int httpsMethodLen = 0;
    int httpsProtocolVersionLen = strlen(HTTPS_PROTOCOL_VERSION);

    /* Check for NULL parameters in the public API. */
    if( ( pReqHandle == NULL) || ( pReqInfo == NULL) )
    {
        IotLogError("NULL parameter passed into IotHttpsClient_InitializeRequest().");
        status = IOT_HTTPS_INVALID_PARAMETER;
    }

    if( status == IOT_HTTPS_OK)
    {
        /* Check of the user buffer is large enough for the request context + default headers. */
        if(pReqInfo->reqUserBuffer.bufferLen < requestUserBufferMinimumSize)
        {
            IotLogError("Buffer size is too small to initialize the request context. User buffer size: %d, required minimum size; %d.", 
                pReqInfo->reqUserBuffer.bufferLen,
                requestUserBufferMinimumSize);
            status = IOT_HTTPS_INSUFFICIENT_MEMORY;
        }
    }

    if( status == IOT_HTTPS_OK )
    {
        /* Set the request contet to the start of the userbuffer. */
        if(pReqInfo->reqUserBuffer.pBuffer != NULL)
        {
            pHttpsRequest = ( _httpsRequest_t *)(pReqInfo->reqUserBuffer.pBuffer);
            /* Clear out the user buffer. */
            memset(pReqInfo->reqUserBuffer.pBuffer, 0, pReqInfo->reqUserBuffer.bufferLen);
        }
        else
        {
            IotLogError("The user buffer pointer IotHttpsRequestInfo_t.reqUserBuffer.pBuffer is NULL.");
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if( status == IOT_HTTPS_OK )
    {
        /* Set the start of the headers to the end of the request context in the user buffer. */
        pHttpsRequest->pHeaders = (uint8_t*)pHttpsRequest + sizeof( _httpsRequest_t );
        pHttpsRequest->pHeadersEnd = (uint8_t*)pHttpsRequest + pReqInfo->reqUserBuffer.bufferLen;
        pHttpsRequest->pHeadersCur = pHttpsRequest->pHeaders;

        /* Get the length of the HTTP method. */
        httpsMethodLen = strlen( _pHttpsMethodStrings[pReqInfo->method] );

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
            status = IOT_HTTPS_INSUFFICIENT_MEMORY;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        /* Write "<METHOD> <PATH> HTTP/1.1\r\n" to the start of the header space. */
        memcpy(pHttpsRequest->pHeadersCur, _pHttpsMethodStrings[pReqInfo->method], httpsMethodLen);
        pHttpsRequest->pHeadersCur += httpsMethodLen;
        memcpy(pHttpsRequest->pHeadersCur, pSpace, spaceLen);
        pHttpsRequest->pHeadersCur += spaceLen;
        if(pReqInfo->pPath == NULL)
        {
            pReqInfo->pPath = HTTPS_EMPTY_PATH;
            pReqInfo->pathLen = strlen(HTTPS_EMPTY_PATH);
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
        status = _addHeader(pHttpsRequest, "User-Agent", IOT_HTTPS_USER_AGENT, strlen( IOT_HTTPS_USER_AGENT ));
        if( status != IOT_HTTPS_OK )
        {
            IotLogError("Failed to write header to the request user buffer: \"User-Agent: %s\\r\\n\" . Error code: %d", 
                IOT_HTTPS_USER_AGENT, 
                status);
        }   
    }

    if( status == IOT_HTTPS_OK )
    {
        /* Check for a NULL IotHttpsRequestInfo_t.pHost. */
        if( pReqInfo->pHost == NULL )
        {
            IotLogError( "NULL IotHttpsRequestInfo_t.pHost was passed into IotHttpsClient_InitializeRequest()." );
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if( status == IOT_HTTPS_OK)
    {
        status = _addHeader(pHttpsRequest, "Host", pReqInfo->pHost, pReqInfo->hostLen);
        if( status != IOT_HTTPS_OK )
        {
            IotLogError("Failed to write \"Host: %.*s\\r\\n\" to the request user buffer. Error code: %d", 
                pReqInfo->hostLen, 
                pReqInfo->pHost, 
                status);
        }
    }

    if( status == IOT_HTTPS_OK )
    {
        if(pReqInfo->isAsync)
        {
            if(pReqInfo->pAsyncInfo != NULL)
            {
                /* If this is an asynchronous request then save the callbacks to use. */
                pHttpsRequest->pCallbacks = &(pReqInfo->pAsyncInfo->callbacks);
                pHttpsRequest->pUserPrivData = pReqInfo->pAsyncInfo->pPrivData;
                /* The body pointer and body length will be filled in when the application sends data in the writeCallback. */
                pHttpsRequest->pBody = NULL;
                pHttpsRequest->bodyLength = 0;
            }
            else
            {
                IotLogError("IotHttpsRequestInfo_t.pAsyncInfo is NULL.");
                status = IOT_HTTPS_INVALID_PARAMETER;
            }

        }
        else
        {
            if(pReqInfo->pSyncInfo != NULL)
            {
                /* If this is a synchronous request then save where the body is stored. */
                pHttpsRequest->pBody = pReqInfo->pSyncInfo->pReqData;
                pHttpsRequest->bodyLength = pReqInfo->pSyncInfo->reqDataLen;
            }
            else
            {
                IotLogError("IotHttpsRequestInfo_t.pSyncInfo is NULL.");
                status = IOT_HTTPS_INVALID_PARAMETER;
            }
        }
    }

    if( status == IOT_HTTPS_OK )
    {
        /* Save the connection info if the connection is to be made at the time of the request. */
        pHttpsRequest->pConnInfo = pReqInfo->pConnInfo;
        /* Set the connection persistence flag for keeping the connection open after receiving a response. */
        pHttpsRequest->isNonPersistent = pReqInfo->isNonPersistent;
        /* Initialize the request to not finished sending. */
        pHttpsRequest->reqFinishedSending = false;

        /* Initialize the corresponding response to this request. */
        if(pReqInfo->respUserBuffer.bufferLen < responseUserBufferMinimumSize)
        {
            IotLogError("Buffer size is too small to initialize the response context associated with this request. User buffer size: %d, required minimum size; %d.", 
                pReqInfo->respUserBuffer.bufferLen, 
                responseUserBufferMinimumSize);
            status = IOT_HTTPS_INSUFFICIENT_MEMORY;
        }
    }

    if( status == IOT_HTTPS_OK)
    {
        if(pReqInfo->respUserBuffer.pBuffer != NULL)
        {
            pHttpsRequest->pHttpsResponse = (_httpsResponse_t *)(pReqInfo->respUserBuffer.pBuffer);
            /* Clear out the response user buffer. */
            memset(pReqInfo->respUserBuffer.pBuffer, 0, pReqInfo->respUserBuffer.bufferLen);
        }
        else
        {
            IotLogError("IotHttpsRequestInfo_t.respUserBuffer.pBuffer is NULL.");
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }
        
    if( status == IOT_HTTPS_OK)
    {
        pHttpsRequest->pHttpsResponse->pHeaders = (uint8_t*)(pHttpsRequest->pHttpsResponse) + sizeof(_httpsResponse_t);
        pHttpsRequest->pHttpsResponse->pHeadersEnd = (uint8_t*)(pHttpsRequest->pHttpsResponse) + pReqInfo->respUserBuffer.bufferLen;
        pHttpsRequest->pHttpsResponse->pHeadersCur = pHttpsRequest->pHttpsResponse->pHeaders;

        /* The request body pointer is allowed to be NULL. pSyncInfo was checked for NULL earlier in this function. */
        if(pReqInfo->isAsync == false)
        {
            pHttpsRequest->pHttpsResponse->pBody = pReqInfo->pSyncInfo->pRespData;
            pHttpsRequest->pHttpsResponse->pBodyCur = pHttpsRequest->pHttpsResponse->pBody;
            pHttpsRequest->pHttpsResponse->pBodyEnd = pHttpsRequest->pHttpsResponse->pBody + pReqInfo->pSyncInfo->respDataLen;
        }
        else
        {
            pHttpsRequest->pHttpsResponse->pBody = NULL;
            pHttpsRequest->pHttpsResponse->pBodyCur = NULL;
            pHttpsRequest->pHttpsResponse->pBodyEnd = NULL;
        }

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

        pHttpsRequest->pHttpsResponse->isAsync = pReqInfo->isAsync;
        pHttpsRequest->pHttpsResponse->pBodyStartInHeaderBuf = NULL;
        pHttpsRequest->pHttpsResponse->bodyLengthInHeaderBuf = 0;
        pHttpsRequest->pHttpsResponse->bodyRxStatus = IOT_HTTPS_OK;
        pHttpsRequest->pHttpsResponse->cancelled = false;
        pHttpsRequest->pHttpsResponse->syncStatus = IOT_HTTPS_OK;
        pHttpsRequest->pHttpsResponse->pHttpsRequest = pHttpsRequest;

        *pReqHandle = pHttpsRequest;
    }

    if((status != IOT_HTTPS_OK) && (pReqHandle != NULL))
    {
        /* Set the request handle to return to NULL, if we failed anywhere. */
        *pReqHandle = NULL;
    }

    return status;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_AddHeader( IotHttpsRequestHandle_t reqHandle, char * pName, char * pValue, uint32_t len )
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    /* Check for NULL pointer paramters. */
    if( (pName == NULL) || (pValue == NULL) || (reqHandle == NULL) )
    {
        IotLogError("Null parameter passed into IotHttpsClient_AddHeader().");
        status = IOT_HTTPS_INVALID_PARAMETER;
    }

    if (status == IOT_HTTPS_OK)
    {
        /* Check for auto-generated header "Content-Length". This header is created and send automatically when right before
        request body is sent on the network. */
        if( strncmp(pName, HTTPS_CONTENT_LENGTH_HEADER, strlen(HTTPS_CONTENT_LENGTH_HEADER)) == 0)
        {
            IotLogError("Attempting to add auto-generated header %s. This is not allowed.", HTTPS_CONTENT_LENGTH_HEADER);
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if (status == IOT_HTTPS_OK)
    {
        /* Check for auto-generated header "Content-Length". This header is created and send automatically when right before
        request body is sent on the network. */
        if( strncmp(pName, HTTPS_CONNECTION_HEADER, strlen(HTTPS_CONNECTION_HEADER)) == 0)
        {
            IotLogError("Attempting to add auto-generated header %s. This is not allowed.", HTTPS_CONNECTION_HEADER);
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        /* Check for auto-generated header "Host". This header is created and placed into the header buffer space
        in IotHttpClient_InitializeRequest(). */
        if( strncmp(pName, HTTPS_HOST_HEADER, strlen(HTTPS_HOST_HEADER)) == 0)
        {
            IotLogError("Attempting to add auto-generated header %s. This is not allowed.", HTTPS_HOST_HEADER);
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        /* Check for auto-generated header "User-Agent". This header is created and placed into the header buffer space
        in IotHttpClient_InitializeRequest(). */
        if(strncmp(pName, HTTPS_USER_AGENT_HEADER, strlen(HTTPS_USER_AGENT_HEADER)) == 0)
        {
            IotLogError("Attempting to add auto-generated header %s. This is not allowed.", HTTPS_USER_AGENT_HEADER);
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        status = _addHeader(reqHandle, pName, pValue, len );
        if(status != IOT_HTTPS_OK)
        {
            IotLogError("Error in IotHttpsClient_AddHeader(), error code %d.", status);
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _networkSend(_httpsConnection_t* pHttpsConnection, uint8_t * pBuf, size_t len)
{
    size_t numBytesSent = 0;
    size_t numBytesSentTotal = 0;
    size_t sendLength = len;
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    /* Send the headers first because the HTTPS body is in a separate pointer these are not contiguous. */
    while( numBytesSentTotal < sendLength )
    {
        numBytesSent = pHttpsConnection->pNetworkInterface->send( pHttpsConnection->pNetworkConnection, 
            &( pBuf[numBytesSentTotal] ), 
            sendLength - numBytesSentTotal );

        if( numBytesSent <= 0 )
        {
            IotLogError("Error in sending the HTTPS headers. Error code: %d", numBytesSent);
            break;
        }

        numBytesSentTotal += numBytesSent;
    }

    if( numBytesSentTotal != sendLength )
    {
        IotLogError("Error sending data on the network. We sent %d but there is %d left to send.", numBytesSentTotal, sendLength);
        status = IOT_HTTPS_NETWORK_ERROR;
    }

    return status;
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _networkRecv( _httpsConnection_t* pHttpsConnection, uint8_t * pBuf, size_t bufLen)
{
    size_t numBytesRecv = 0;
    size_t numBytesRecvTotal = 0;
    size_t lengthToReceive = bufLen;
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    do {
        numBytesRecv = pHttpsConnection->pNetworkInterface->receive( pHttpsConnection->pNetworkConnection,
            &( pBuf[ numBytesRecvTotal ] ),
            lengthToReceive - numBytesRecvTotal );

        if( numBytesRecv > 0 )
        {
            numBytesRecvTotal += numBytesRecv;
        }
        if( numBytesRecv == 0)
        {
            IotLogError("Timedout waiting for the HTTPS response message.");
            status = IOT_HTTPS_TIMEOUT_ERROR;
            break;
        }
        if( numBytesRecv < 0)
        {
            IotLogError("Error in receiving the HTTPS response message. Socket Error code %d", numBytesRecv);
            status = IOT_HTTPS_NETWORK_ERROR;
            break;
        }
    } while((numBytesRecv > 0)  && ( lengthToReceive - numBytesRecvTotal > 0));

    return status;   
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _sendHttpsHeaders( _httpsConnection_t* pHttpsConnection, uint8_t* pHeadersBuf, uint32_t headersLength, bool isNonPersistent, uint32_t contentLength)
{
    const char* connectionHeader = NULL;
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    int numWritten = 0;
    int connectionHeaderLen = 0;
    /* The Content-Length header of the form "Content-Length: N\r\n" with a NULL terminator for snprintf. */
    char contentLengthHeaderStr[HTTPS_MAX_CONTENT_LENGTH_LINE_LENGTH + 1];
    /* The HTTP headers to send after the headers in pHeadersBuf are the Content-Length and the Connection type and
    the final "\r\n" to indicate the end of the the header lines. */
    char finalHeaders[HTTPS_MAX_CONTENT_LENGTH_LINE_LENGTH + HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE_LENGTH + HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH] = { 0 };
    
    /* Send the headers passed into this function first. These headers are not termined with a second set of "\r\n". */
    status = _networkSend( pHttpsConnection, pHeadersBuf, headersLength);
    if( status != IOT_HTTPS_OK )
    {
        IotLogError("Error sending the HTTPS headers in the request user buffer. Error code: %d", status);
    }

    if(status == IOT_HTTPS_OK)
    {
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
            status = IOT_HTTPS_INTERNAL_ERROR;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        /* snprintf() succeeded so copy that to the finalHeaders. */
        memcpy(finalHeaders, contentLengthHeaderStr, numWritten);
        /* Write the connection persistence type to the final headers. */
        if (isNonPersistent)
        {
            connectionHeader = HTTPS_CONNECTION_CLOSE_HEADER_LINE;
            connectionHeaderLen = strlen(HTTPS_CONNECTION_CLOSE_HEADER_LINE);
        }
        else
        {
            connectionHeader = HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE;
            connectionHeaderLen = strlen(HTTPS_CONNECTION_KEEP_ALIVE_HEADER_LINE);
        }
        memcpy(&finalHeaders[numWritten], connectionHeader, connectionHeaderLen);
        numWritten += connectionHeaderLen;
        memcpy( &finalHeaders[numWritten], HTTPS_END_OF_HEADER_LINES_INDICATOR, HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH );
        numWritten += HTTPS_END_OF_HEADER_LINES_INDICATOR_LENGTH;

        status = _networkSend( pHttpsConnection, (uint8_t*)finalHeaders, numWritten);
        if( status != IOT_HTTPS_OK )
        {
            IotLogError("Error sending final HTTPS Headers \r\n%s. Error code: %d", finalHeaders, status);
        }
    }

    return status;

}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _sendHttpsBody( _httpsConnection_t* pHttpsConnection, uint8_t* pBodyBuf, uint32_t bodyLength)
{
    IotHttpsReturnCode_t status = _networkSend( pHttpsConnection, pBodyBuf, bodyLength);
    if( status != IOT_HTTPS_OK )
    {
        IotLogError("Error sending final HTTPS body at location 0x%x. Error code: %d", pBodyBuf, status);
    }
    return status;
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _parseHttpsMessage(_httpParserInfo_t* pHttpParserInfo, char* pBuf, size_t len)
{
    size_t parsedBytes = 0;
    const char * pHttpParserErrorDescription = NULL;
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    http_parser *pHttpParser = &(pHttpParserInfo->parser);

    parsedBytes = pHttpParserInfo->parseFunc( pHttpParser, &_httpParserSettings, pBuf, len );
    IotLogDebug( "http-parser parsed %d bytes out of %d specified.", parsedBytes, len );

    /* If the parser fails with HPE_CLOSED_CONNECTION or HPE_INVALID_CONSTANT that simply means there
       was data beyond the end of the message. We do not fail in this case because we often give the whole
       header buffer or body buffer to the parser even if it is only partly filled with data. 
       The error must also not be because if exiting the parser early. Errors <= CB_chunk_complete mean a 
       non-zero number was returned from some callback. I return a nonzero from the callback when I want to stop the 
       parser early like for a HEAD request. */
    if( (pHttpParser->http_errno != 0) &&
        ( HTTP_PARSER_ERRNO( pHttpParser ) != HPE_CLOSED_CONNECTION ) &&
        ( HTTP_PARSER_ERRNO( pHttpParser ) != HPE_INVALID_CONSTANT ) &&
        ( HTTP_PARSER_ERRNO( pHttpParser ) > HPE_CB_chunk_complete) )
    {
        pHttpParserErrorDescription = http_errno_description( HTTP_PARSER_ERRNO( pHttpParser ) );
        IotLogError("http_parser failed on the http response with error: %s", pHttpParserErrorDescription);
        status = IOT_HTTPS_PARSING_ERROR;
    }

    return status;
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _receiveHttpsMessage( _httpsConnection_t* pHttpsConnection, 
    _httpParserInfo_t *pHttpParserInfo,
    IotHttpsResponseParserState_t *pCurrentParserState,
    IotHttpsResponseParserState_t finalParserState, 
    uint8_t** pBuf,
    uint8_t** pBufCur,
    uint8_t** pBufEnd,
    IotHttpsReturnCode_t *pNetworkStatus)
{    
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    while( (*pCurrentParserState < finalParserState) && (*pBufEnd - *pBufCur > 0) ) 
    {
        *pNetworkStatus = _networkRecv( pHttpsConnection,
            *pBufCur, 
            *pBufEnd - *pBufCur);

        status = _parseHttpsMessage(pHttpParserInfo, (char*)(*pBufCur), *pBufEnd - *pBufCur);
        if(status != IOT_HTTPS_OK)
        {
            IotLogError("Failed to parse the message buffer with error: %d", pHttpParserInfo->parser.http_errno);
            break;
        }

        /* The _httResponse->pHeadersCur pointer is updated in the http_parser callbacks. */
        IotLogDebug( "There is %d of space left in the buffer.", *pBufEnd - *pBufCur );

        /* We cannot anticipate the unique network error received when the server closes the connection. 
           We simply exit the loop if there nothing else to receive. We do not return the network status because 
           the error may just be a server closed the connection, but there may still have been HTTP data in the buffer
           that we parsed. */
        if( *pNetworkStatus != IOT_HTTPS_OK )
        {
            IotLogError( "Network error receiving the HTTPS response headers. Error code: %d", *pNetworkStatus );
            break;
        }

    }

    /* If we did not reach the end of the headers or body in the parser callbacks, then the buffer configured does not
       fit all of that part of the HTTP message. */
    if( *pCurrentParserState < finalParserState)
    {
        IotLogDebug("There are still more data on the network. It could not fit into buffer at location 0x%x with length %d.",
            *pBuf,
            *pBufEnd - *pBuf);
    }

    return status;
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _receiveHttpsHeaders( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t *pNetworkStatus)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    status = _receiveHttpsMessage(pHttpsConnection,
        &(pHttpsResponse->httpParserInfo),
        &(pHttpsResponse->parserState),
        PARSER_STATE_HEADERS_COMPLETE,
        &(pHttpsResponse->pHeaders),
        &(pHttpsResponse->pHeadersCur),
        &(pHttpsResponse->pHeadersEnd),
        pNetworkStatus);
    if( status != IOT_HTTPS_OK)
    {
        IotLogError("Error receiving the HTTP headers. Error code %d", status);
    }

    return status;
}

/*-----------------------------------------------------------*/

/* _receiveHttpsHeaders() must be called first before this function is called. */
static IotHttpsReturnCode_t _receiveHttpsBody( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse, IotHttpsReturnCode_t *pNetworkStatus)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    pHttpsResponse->bufferProcessingState = PROCESSING_STATE_FILLING_BODY_BUFFER;

    status = _receiveHttpsMessage(pHttpsConnection,
        &(pHttpsResponse->httpParserInfo),
        &(pHttpsResponse->parserState),
        PARSER_STATE_BODY_COMPLETE,
        &(pHttpsResponse->pBody),
        &(pHttpsResponse->pBodyCur),
        &(pHttpsResponse->pBodyEnd),
        pNetworkStatus);
    if( status != IOT_HTTPS_OK)
    {
        IotLogError("Error receiving the HTTP body. Error code %d", status);
    }



    IotLogDebug("The message Content-Length is %d (Will be > 0 for a Content-Length header existing). The remaining content length on the network is %d.",
        pHttpsResponse->contentLength,
        pHttpsResponse->httpParserInfo.parser.content_length);

    return status;
}

/*-----------------------------------------------------------*/

static IotHttpsReturnCode_t _flushHttpsNetworkData( _httpsConnection_t* pHttpsConnection, _httpsResponse_t* pHttpsResponse )
{
    static uint8_t flushBuffer[IOT_HTTPS_MAX_FLUSH_BUFFER_SIZE] = { 0 };
    const char * pHttpParserErrorDescription = NULL;
    IotHttpsReturnCode_t parserStatus = IOT_HTTPS_OK;
    IotHttpsReturnCode_t networkStatus = IOT_HTTPS_OK;
    IotHttpsReturnCode_t returnStatus = IOT_HTTPS_OK;

    /* Even if there is not body, the parser state will become body complete after the headers finish. */
    while( pHttpsResponse->parserState < PARSER_STATE_BODY_COMPLETE )
    {
        IotLogDebug( "Now clearing the rest of the response data on the socket. " );
        networkStatus = _networkRecv( pHttpsConnection, flushBuffer, IOT_HTTPS_MAX_FLUSH_BUFFER_SIZE );

        /* Run this through the parser so that we can get the end of the HTTP message, instead of simply timing out the socket to stop.
           If we relied on the socket timeout to stop reading the network socket, then the server may close the connection. */
        parserStatus = _parseHttpsMessage(&(pHttpsResponse->httpParserInfo), (char*)flushBuffer, IOT_HTTPS_MAX_FLUSH_BUFFER_SIZE );
        if(parserStatus != IOT_HTTPS_OK)
        {
            pHttpParserErrorDescription = http_errno_description( HTTP_PARSER_ERRNO( &pHttpsResponse->httpParserInfo.parser ) );
            IotLogError("Network Flush: Failed to parse the response body buffer with error: %d", pHttpsResponse->httpParserInfo.parser.http_errno);
            break;
        }

        /* If there is a network error then we want to stop clearing out the buffer. */
        if( networkStatus != IOT_HTTPS_OK )
        {
            IotLogWarn( "Network Flush: Error receiving the rest of the HTTP response. Error code: %d", 
                networkStatus );
            break;
        }
    }

    /* All network errors except timeout are returned. */
    if( networkStatus != IOT_HTTPS_TIMEOUT_ERROR)
    {
        returnStatus = networkStatus;
    }
    else
    {
        returnStatus = parserStatus;
    }

    /* If there is a timeout error just return the parser status. */
    return returnStatus;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_SendSync(IotHttpsConnectionHandle_t *pConnHandle, IotHttpsRequestHandle_t reqHandle, IotHttpsResponseHandle_t * pRespHandle, uint32_t timeoutMs)
{

    IOT_FUNCTION_ENTRY(IotHttpsReturnCode_t, IOT_HTTPS_OK);

    IotHttpsReturnCode_t flushStatus = IOT_HTTPS_OK;
    IotHttpsReturnCode_t networkStatus = IOT_HTTPS_OK;
    bool respFinishedSemCreated = false;
    _httpsResponse_t* pHttpsResponse = NULL;

    /* Check for NULL parameters in a public API. */
    if(( pConnHandle == NULL) || (reqHandle == NULL) || (pRespHandle == NULL))
    {
        IotLogError("NULL parameter passed into IotHttpsClient_SendSync()");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INVALID_PARAMETER);
    }

    if(reqHandle->pHttpsResponse == NULL)
    {
        IotLogError("Null response handle associated with the input reqHandle to IotHttpsClient_SendSync().");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INVALID_PARAMETER);
    }

    /* If an asynchronous request/response is configured, that is invalid for this API. */
    if(reqHandle->pHttpsResponse->isAsync)
    {
        IotLogError("Called IotHttpsClient_SendSync on an asynchronous configured request.");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INVALID_PARAMETER);
    }

    /* This routine will set the pConnHandle to return if successful. */
    status = _implicitlyConnect(pConnHandle, reqHandle->pConnInfo);
    if(status != IOT_HTTPS_OK)
    {
        IotLogError("Failed to connect implicitly in IotHttpsClient_SendSync. Error code: %d", status);
        IOT_GOTO_CLEANUP();
    }

    /* Set the response handle to return. */
    *pRespHandle = reqHandle->pHttpsResponse;

    /* Set the internal response to use. */
    pHttpsResponse = *pRespHandle;
    
    /* Otherwise the implicit connection passed and we need to the set the connection handle in the request and response. */
    reqHandle->pHttpsConnection = *pConnHandle;
    pHttpsResponse->pHttpsConnection = *pConnHandle;

    /* Create the semaphore used to wait on the response to finish being received. */
    respFinishedSemCreated = IotSemaphore_Create( &( pHttpsResponse->respFinishedSem ), 0, 1 );
    if( respFinishedSemCreated == false  )
    {
        IotLogError("Failed to create an internal semaphore.");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INTERNAL_ERROR);
    }

    /* Schedule this request to be sent by adding it to the connection's request queue. */
    status = _addRequestToConnectionReqQ(reqHandle);

    if(status != IOT_HTTPS_OK)
    {
        IotLogError("Failed to schedule the synchronous request. Error code: %d", status);
        IOT_GOTO_CLEANUP();
    }

    /* Wait for the request to finish. */
    if( timeoutMs == 0)
    {
        IotSemaphore_Wait(&(pHttpsResponse->respFinishedSem));
    }
    else
    {
        if( IotSemaphore_TimedWait( &(pHttpsResponse->respFinishedSem), timeoutMs ) == false )
        {
            IotLogError("Timed out waiting for the synchronous request to finish. Timeout ms: %d", timeoutMs);
            IotHttpsClient_CancelRequestAsync(reqHandle, *pRespHandle);
            IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_BUSY);
        }
    }

    IOT_FUNCTION_CLEANUP_BEGIN();
    if(respFinishedSemCreated)
    {
        IotSemaphore_Destroy(&(pHttpsResponse->respFinishedSem));
    }
    else
    {
        IotLogDebug("Received network error when flushing the socket. Error code: %d", flushStatus);
    }

    /* If this function failed, then the request was never scheduled. */
    if(status != IOT_HTTPS_OK)
    {
        pHttpsResponse->syncStatus = status;
    }

    /* If we failed during the _networkReceiveCallback() or _sendHttpsRequest() routines. */
    if(pHttpsResponse->syncStatus != IOT_HTTPS_OK)
    {
        status = pHttpsResponse->syncStatus;
        *pRespHandle = NULL;
        IotLogError("IotHttpsClient_SendSync() failed.");
    }

    IOT_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_WriteRequestBody(IotHttpsRequestHandle_t reqHandle, char *pBuf, uint32_t len, int isComplete)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    /* With the current HTTP/1.1 we enforce that isComplete must be set to 1. */
    if(isComplete != 1)
    {
        IotLogError("isComplete must be 1 in IotHttpsClient_WriteRequestBody() for the current version of the HTTPS Client library.");
        status = IOT_HTTPS_NOT_SUPPORTED;
    }

    /* Check for a NULL reqHandle. */
    if(status == IOT_HTTPS_OK)
    {
        if(reqHandle->pHttpsResponse == NULL)
        {
            IotLogError("Null response handle associated with the input reqHandle to IotHttpsClient_WriteRequestBody().");
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        if(reqHandle->pHttpsResponse->isAsync == false)
        {
            /* This function is not valid for a synchronous response. Applications need to configure the request body in 
               IotHttpsRequestInfo_t.pSyncInfo_t.reqData before calling IotHttpsClient_SendSync(). */
            IotLogError("Called IotHttpsClient_ReadResponseBody() on a synchronous response. This is valid only for an asynchronous response.");
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        /* If the bodyLength is greater than 0, then we already called this function and we need to enforce that 
           function must only be called once. We only call this function once so that we can calculate the Content-Length
           easily. */
        if(reqHandle->bodyLength > 0)
        {
            IotLogError("Error this function must be called once with the data needed to send. Variable length HTTP "
                "request body is not supported in this library.");
            status = IOT_HTTPS_MESSAGE_FINISHED;
        }
    }

    /* Set the pointer to the body and the length for the content-length calculation. */
    if(status == IOT_HTTPS_OK)
    {
        reqHandle->pBody = pBuf;
        reqHandle->bodyLength = len;
    }

    return status;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_ReadResponseBody(IotHttpsResponseHandle_t respHandle, uint8_t * pBuf, uint32_t *pLen)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    if((respHandle == NULL) || (pBuf == NULL) || (pLen == NULL))
    {
        IotLogError("NULL parameter passed into IotHttpsClient_ReadResponseBody()");
        status = IOT_HTTPS_INVALID_PARAMETER;
    }

    if(status == IOT_HTTPS_OK)
    {
        if(respHandle->isAsync == false)
        {
            /* This is not valid for a synchronous response. Synchronous requests need to reference the pBuffer in 
            IotHttpsRequestInfo_t.pSyncInfo_t.respData for the response body. */
            IotLogError("Called IotHttpsClient_ReadResponseBody() on a synchronous response. This is valid only for an asynchronous response.");
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        /* Set the current body in the respHandle to use in _receiveHttpsBody(). _receiveHttpsBody is generic
           to both async and sync request/response handling. In the sync version the body is configured during
           initializing the request. In the async version the body is given in this function on the fly. */
        respHandle->pBody = pBuf;
        respHandle->pBodyCur = respHandle->pBody;
        respHandle->pBodyEnd = respHandle->pBodyCur + *pLen;
        /* When there is part of the body in the header pBuffer. We need to move that data to this body pBuffer 
           provided in this fuction. */
        if(respHandle->pBodyStartInHeaderBuf != NULL)
        {
            uint32_t copyLength = respHandle->bodyLengthInHeaderBuf > *pLen ? *pLen : respHandle->bodyLengthInHeaderBuf;
            memcpy(respHandle->pBodyCur, respHandle->pBodyStartInHeaderBuf, copyLength);
            respHandle->pBodyCur += copyLength;
        }
        if((respHandle->pBodyEnd - respHandle->pBodyCur) > 0)
        {
            status = _receiveHttpsBody(respHandle->pHttpsConnection, respHandle, &(respHandle->bodyRxStatus) );
        }
        *pLen = respHandle->pBodyCur - respHandle->pBody;
    }

    return status;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_CancelRequestAsync(IotHttpsRequestHandle_t reqHandle, IotHttpsResponseHandle_t respHandle)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    if( respHandle != NULL )
    {
        respHandle->cancelled = true;
    }
    else if( reqHandle != NULL )
    {
        reqHandle->cancelled = true;
    }
    else
    {
        IotLogError( "Both parameters to IotHttpsClient_CancelRequestAsync are NULL." );
        status = IOT_HTTPS_INVALID_PARAMETER;
    }
    return status;
}

/*-----------------------------------------------------------*/

static void _sendHttpsRequest( IotTaskPool_t pTaskPool, IotTaskPoolJob_t pJob, void * pUserContext )
{
    IOT_FUNCTION_ENTRY( IotHttpsReturnCode_t, IOT_HTTPS_OK );

    _httpsRequest_t* pHttpsRequest = (_httpsRequest_t*)( pUserContext );
    _httpsConnection_t* pHttpsConnection = pHttpsRequest->pHttpsConnection;
    _httpsResponse_t* pHttpsResponse = pHttpsRequest->pHttpsResponse;
    IotHttpsReturnCode_t flushStatus = IOT_HTTPS_OK;

    IotLogDebug( "Task with request ID: %d started.", pHttpsRequest );

    if(pHttpsResponse->cancelled == true)
    {
        IotLogDebug("Request ID: %d was cancelled.", pHttpsRequest );
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_ASYNC_CANCELLED);
    }

    /* Queue the response to expect from the network. */
    IotMutex_Lock(&(pHttpsConnection->respQMutex));
    IotDeQueue_EnqueueTail(&(pHttpsConnection->respQ), &(pHttpsResponse->link));
    IotMutex_Unlock(&(pHttpsConnection->respQMutex));

    /* After queuing to protect against out of order network data from a rouge server signal that the request is 
       not finished sending. */
    pHttpsRequest->reqFinishedSending = false;

    /* Get the headers from the application. For a synchronous request we should have appended extra headers before
       we got to this point. */
    if(pHttpsResponse->isAsync && pHttpsRequest->pCallbacks->appendHeaderCallback)
    {
        pHttpsRequest->pCallbacks->appendHeaderCallback(pHttpsRequest->pUserPrivData, pHttpsRequest);
    }

    if(pHttpsResponse->cancelled == true)
    {
        IotLogDebug("Request ID: %d was cancelled.", pHttpsRequest );
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_ASYNC_CANCELLED);
    }

    /* Ask the user for data to write body to the network. We only ask the user once. This is so that
       we can calculate the Content-Length to send.*/
    if(pHttpsResponse->isAsync && pHttpsRequest->pCallbacks->writeCallback)
    {
        /* If there is data, then a Content-Length header value will be provided and we send the headers
           before that user data. */
        pHttpsRequest->pCallbacks->writeCallback(pHttpsRequest->pUserPrivData, pHttpsRequest);
    }

    /* Send the HTTP headers. */
    status = _sendHttpsHeaders(pHttpsConnection,
        pHttpsRequest->pHeaders,
        pHttpsRequest->pHeadersCur - pHttpsRequest->pHeaders,
        pHttpsRequest->isNonPersistent,
        pHttpsRequest->bodyLength);
    if( status != IOT_HTTPS_OK )
    {
        IotLogError("Error sending the HTTPS headers with error code: %d", status);
        IOT_SET_AND_GOTO_CLEANUP(status);
    }

    if((pHttpsRequest->pBody != NULL) && (pHttpsRequest->bodyLength > 0))
    {
        status = _sendHttpsBody( pHttpsConnection, pHttpsRequest->pBody, pHttpsRequest->bodyLength);
        if( status != IOT_HTTPS_OK )
        {
            IotLogError("Error sending final HTTPS body. Return code: %d", status);
            IOT_GOTO_CLEANUP(); 
        }
    }

    if(pHttpsResponse->cancelled == true)
    {
        IotLogDebug("Request ID: %d was cancelled.", pHttpsRequest );
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_ASYNC_CANCELLED);
    }

    IOT_FUNCTION_CLEANUP_BEGIN();

    /* The request has finished sending. */
    pHttpsRequest->reqFinishedSending = true;

    /* Return the possible error to the application. */
    if( status != IOT_HTTPS_OK )
    {
        pHttpsRequest->pCallbacks->errorCallback( pHttpsRequest->pUserPrivData, pHttpsRequest, status );
    }
}

/* --------------------------------------------------------- */

static IotHttpsReturnCode_t _implicitlyConnect(IotHttpsConnectionHandle_t *pConnHandle, IotHttpsConnectionInfo_t* pConnInfo)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    /* If the pConnHandle points to a NULL handle or the pConnHandle is false, then make the connection now. */
    if( (* pConnHandle == NULL) || ((* pConnHandle)->isConnected == false ))
    {
        /* In order to make the connection now the pConnInfo member of IotHttpsRequestHandle_t must not be NULL. */
        if(pConnInfo == NULL)
        {
            IotLogError("IotHttpsRequestInfo_t should have been configured with pConnInfo not NULL in IotHttpsClient_InitializeRequest() in order to connect implicitly.");
            status = IOT_HTTPS_INVALID_PARAMETER;
        }

        if( status == IOT_HTTPS_OK)
        {
            /* This routine will set the pConnHandle to return if successful. */
            status = _createHttpsConnection(pConnHandle, pConnInfo);
            if(status != IOT_HTTPS_OK)
            {
                IotLogError("An error occurred in connecting with th server with error code: %d", status);
            }
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t _scheduleHttpsRequestSend(_httpsRequest_t* pHttpsRequest)
{
    IotTaskPoolError_t taskPoolStatus = IOT_TASKPOOL_SUCCESS;
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    _httpsConnection_t* pHttpsConnection = pHttpsRequest->pHttpsConnection;

    taskPoolStatus = IotTaskPool_CreateJob( _sendHttpsRequest,
        ( void* )(pHttpsRequest),
        &( pHttpsConnection->taskPoolJobStorage ),
        &( pHttpsConnection->taskPoolJob ) );

    /* Creating a task pool job should never fail when parameters are valid. */
    if( taskPoolStatus != IOT_TASKPOOL_SUCCESS )
    {
        IotLogError( "Error creating a taskpool job for request servicing. Error code: %d", taskPoolStatus );
        status = IOT_HTTPS_INTERNAL_ERROR;
    }

    if(status == IOT_HTTPS_OK)
    {
        taskPoolStatus = IotTaskPool_Schedule( IOT_SYSTEM_TASKPOOL, pHttpsConnection->taskPoolJob, 0);
        if( taskPoolStatus != IOT_TASKPOOL_SUCCESS )
        {
            IotLogError( "Failed to schedule taskpool job. Error code: %d", taskPoolStatus );
            status = IOT_HTTPS_ASYNC_SCHEDULING_ERROR;
        }
    }

    return status;  
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t _addRequestToConnectionReqQ(_httpsRequest_t* pHttpsRequest)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;
    _httpsConnection_t* pHttpsConnection = pHttpsRequest->pHttpsConnection;
    bool isOnlyRequest = false;

    /* Place the request into the queue. */
    IotMutex_Lock( &( pHttpsConnection->reqQMutex ) );

    /* If this is the first and only item in the list, then we will want to schedule a new task to service this 
        request. If this is the first and only item in the list, then there is no task currently sending a request 
        and there is no response currently being received. */
    if(IotDeQueue_IsEmpty(&( pHttpsConnection->reqQ )))
    {
        isOnlyRequest = true;
    }

    IotDeQueue_EnqueueTail(&( pHttpsConnection->reqQ ), &(pHttpsRequest->link));

    IotMutex_Unlock( &(pHttpsConnection->reqQMutex) );

    if(isOnlyRequest)
    {
        status = _scheduleHttpsRequestSend(pHttpsRequest);
        if( status != IOT_HTTPS_OK )
        {
            IotLogError( "Failed to schedule the only request in the queue for request %d. Error code: %d", pHttpsRequest, status );
        }
    }

    return status;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_SendAsync(IotHttpsConnectionHandle_t *pConnHandle, IotHttpsRequestHandle_t reqHandle, IotHttpsResponseHandle_t * pRespHandle)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    /* Check for NULL inputs parameters. */
    if(( pConnHandle == NULL) || (reqHandle == NULL) || (pRespHandle == NULL))
    {
        IotLogError("NULL parameter passed into IotHttpsClient_SendAsync()");
        status = IOT_HTTPS_INVALID_PARAMETER;
    }

    if(status == IOT_HTTPS_OK)
    {
        if(reqHandle->pHttpsResponse->isAsync == false)
        {
            IotLogError("Called IotHttpsClient_SendAsync on a synchronous configured request.");
            status = IOT_HTTPS_INVALID_PARAMETER;
        }
    }

    if(status == IOT_HTTPS_OK)
    {
        /* Connect implicitly if we need to. This will will return a valid pRespHandle and a valid pConnHandle. */
        status = _implicitlyConnect(pConnHandle, reqHandle->pConnInfo);
        if(status != IOT_HTTPS_OK)
        {
            IotLogError("Failed to connect implicitly in IotHttpsClient_SendAsync. Error code: %d", status);
        }
        else
        {
            if(reqHandle->pCallbacks->connectionEstablishedCallback)
            {
                reqHandle->pCallbacks->connectionEstablishedCallback(reqHandle->pUserPrivData, *pConnHandle, status);
            }
        }

    }

    if(status == IOT_HTTPS_OK)
    {
        /* Set the connection handle in the request handle so that we can use it in the _writeResponseBody() callback. */
        reqHandle->pHttpsConnection = *pConnHandle;

        /* Set the response handle to return. */
        *pRespHandle = reqHandle->pHttpsResponse;

        /* Set the connection handle in the response handle sp that we can use it in the _readReadyCallback() callback. */
        ( *pRespHandle )->pHttpsConnection = *pConnHandle;

        /* Add the request to the connection's request queue. */
        status = _addRequestToConnectionReqQ(reqHandle);
        if(status != IOT_HTTPS_OK)
        {
            IotLogError("Failed to add request %d to the connection's request queue. Error code: %d.", reqHandle, status);
        }
    }
    return status;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_ReadResponseStatus(IotHttpsResponseHandle_t respHandle, uint16_t *pStatus)
{
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    if((respHandle == NULL) || (pStatus == NULL))
    {
        IotLogError("NULL parameter passed into IotHttpsClient_ReadResponseStatus().");
        status = IOT_HTTPS_INVALID_PARAMETER;
    }

    if( status == IOT_HTTPS_OK)
    {
        if( respHandle->status == 0)
        {
            IotLogError("The HTTP response status was not found in the HTTP response header buffer.");
            return IOT_HTTPS_NOT_FOUND;
        }
        *pStatus = respHandle->status;
    }

    
    return status;
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_ReadHeader(IotHttpsResponseHandle_t respHandle, char *pName, char *pValue, uint32_t len)
{
    IOT_FUNCTION_ENTRY(IotHttpsReturnCode_t, IOT_HTTPS_OK);
    const char * pHttpParserErrorDescription = NULL;
    IotHttpsResponseBufferState_t savedState = PROCESSING_STATE_NONE;
    size_t numParsed = 0;

    IotLogDebug("IotHttpsClient_ReadHeader(): Attempting to find the header %s. This was received on requests %d.",
        pName,
        respHandle->pHttpsRequest);

    if((respHandle == NULL) || (pName == NULL) || (pValue == NULL))
    {
        IotLogError("NULL parameter passed into IotHttpsClient_ReadResponseStatus().");
        IOT_SET_AND_GOTO_CLEANUP(IOT_HTTPS_INVALID_PARAMETER);
    }

    /* Save the current state of the response's buffer processing. */
    savedState = respHandle->bufferProcessingState;

    respHandle->pReadHeaderField = pName;
    respHandle->foundHeaderField = false;
    respHandle->bufferProcessingState = PROCESSING_STATE_SEARCHING_HEADER_BUFFER;

    http_parser_init( &( respHandle->httpParserInfo.parser ), HTTP_RESPONSE );
    respHandle->httpParserInfo.parser.data = (void *)(respHandle);
    numParsed = respHandle->httpParserInfo.parseFunc(&(respHandle->httpParserInfo.parser), &_httpParserSettings, (char*)(respHandle->pHeaders), respHandle->pHeadersCur - respHandle->pHeaders);
    IotLogDebug("Parsed %d characters in IotHttpsClient_ReadHeader().", numParsed);
    if( (respHandle->httpParserInfo.parser.http_errno != 0) && 
        ( HTTP_PARSER_ERRNO( &(respHandle->httpParserInfo.parser) ) > HPE_CB_chunk_complete) )
    {
        pHttpParserErrorDescription = http_errno_description( HTTP_PARSER_ERRNO( &(respHandle->httpParserInfo.parser) ) );
        IotLogError("http_parser failed on the http response with error: %s", pHttpParserErrorDescription);
        IOT_SET_AND_GOTO_CLEANUP( IOT_HTTPS_PARSING_ERROR);
    }

    if(respHandle->foundHeaderField)
    {
        if(respHandle->readHeaderValueLength > len)
        {
            IotLogError("IotHttpsClient_ReadHeader(): The length of the pValue buffer specified is less than the actual length of the pValue. ");
            IOT_SET_AND_GOTO_CLEANUP( IOT_HTTPS_INSUFFICIENT_MEMORY );
        }
        else
        {
            /* stncpy adds a NULL terminator. */
            strncpy(pValue, respHandle->pReadHeaderValue, respHandle->readHeaderValueLength);
        }
    }
    else
    {
        IotLogError("IotHttpsClient_ReadHeader(): The header field %s was not found.", pName);
        IOT_SET_AND_GOTO_CLEANUP( IOT_HTTPS_NOT_FOUND );
    }

    IOT_FUNCTION_CLEANUP_BEGIN();
    /* Always restore the state back to what it was before entering this function. */
    if( respHandle != NULL )
    {
        respHandle->bufferProcessingState = savedState;
    }
    IOT_FUNCTION_CLEANUP_END();
}

/*-----------------------------------------------------------*/

IotHttpsReturnCode_t IotHttpsClient_ReadContentLength(IotHttpsResponseHandle_t respHandle, uint32_t *pContentLength)
{   
    IotHttpsReturnCode_t status = IOT_HTTPS_OK;

    /* Check for NULL parameters in this public API. */
    if((respHandle == NULL) || (pContentLength == NULL))
    {
        IotLogError("NULL parameter passed into IotHttpsClient_ReadContentLength().");
        status = IOT_HTTPS_INVALID_PARAMETER;
    }

    if( status == IOT_HTTPS_OK)
    {
        /* If there is no content-length header or if we were not able to store it in the header buffer this will be
        invalid. */
        if(respHandle->contentLength <= 0)
        {
            IotLogError("The content length not found in the HTTP response header buffer.");
            *pContentLength = 0;
            status = IOT_HTTPS_NOT_FOUND;
        }
        else
        {
            *pContentLength = respHandle->contentLength;
        }
    }

    return status;
}

/*-----------------------------------------------------------*/
