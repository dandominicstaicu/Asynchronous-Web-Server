/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef AWS_H_
#define AWS_H_		1

#include "http-parser/http_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AWS_LISTEN_PORT		8888
#define AWS_DOCUMENT_ROOT	"./"
#define AWS_REL_STATIC_FOLDER	"static/"
#define AWS_REL_DYNAMIC_FOLDER	"dynamic/"
#define AWS_ABS_STATIC_FOLDER	(AWS_DOCUMENT_ROOT AWS_REL_STATIC_FOLDER)
#define AWS_ABS_DYNAMIC_FOLDER	(AWS_DOCUMENT_ROOT AWS_REL_DYNAMIC_FOLDER)

#define ERROR_MSG                                                              \
	"HTTP/1.1 404 Not Found\r\n"                                           \
	"Date: Mon, 30 May 2022 16:00:00 GMT\r\n"                              \
	"Server: Neard/9.9.9\r\n"                                              \
	"Last-Modified: Mon, 1 May 2022 15:00:00 GMT\r\n"                      \
	"Accept-Ranges: bytes\r\n"                                             \
	"Content-Length: 0\r\n"                                                \
	"Vary: Accept-Encoding\r\n"                                            \
	"Connection: close\r\n"                                                \
	"Content-Type: text/html\r\n"                                          \
	"\r\n"

#define SUCCESS_MSG                                                                 \
	"HTTP/1.1 200 OK\r\n"                                                  \
	"Date: Mon, 30 May 2022 16:00:00 GMT\r\n"                              \
	"Server: Neard/9.9.9\r\n"                                              \
	"Last-Modified: 1 May 2022 15:00:00 GMT\r\n"                           \
	"Accept-Ranges: bytes\r\n"                                             \
	"Content-Length: %ld\r\n"                                              \
	"Vary: Accept-Encoding\r\n"                                            \
	"Connection: close\r\n"                                                \
	"Content-Type: text/html\r\n"                                          \
	"\r\n"


enum connection_state {
	STATE_INITIAL,
	STATE_RECEIVING_DATA,
	STATE_REQUEST_RECEIVED,
	STATE_SENDING_DATA,
	STATE_SENDING_HEADER,
	STATE_SENDING_404,
	STATE_ASYNC_ONGOING,
	STATE_DATA_SENT,
	STATE_HEADER_SENT,
	STATE_404_SENT,
	STATE_CONNECTION_CLOSED,
	STATE_NO_STATE // Used for assignment skelethon
};

#define OUT_STATE(s) (((s) == STATE_SENDING_DATA) ||	\
	((s) == STATE_SENDING_HEADER) || ((s) == STATE_SENDING_404))

/* Resource type request by HTTP (either static or dynamic) */
enum resource_type {
	RESOURCE_TYPE_NONE,
	RESOURCE_TYPE_STATIC,
	RESOURCE_TYPE_DYNAMIC
};

/* Structure acting as a connection handler */
struct connection {
    /* file to be sent */
	int fd; // file fd
	char filename[BUFSIZ];

    /* asynchronous notification */
	int eventfd;
	int sockfd;

	io_context_t ctx;
	struct iocb *iocb; // iocb
	struct iocb **piocb; // *piocb[1]
	size_t file_size;

	/* buffers used for receiving messages */
	char recv_buffer[BUFSIZ];
	size_t recv_len;

	/* Used for sending data (headers, 404 or data populated through async IO). */
	char send_buffer[BUFSIZ];
	size_t send_len;
	size_t send_pos;
	size_t file_pos;
	size_t async_read_len;

	/* HTTP request path */
	int have_path;
	char request_path[BUFSIZ];
	enum resource_type res_type;
	enum connection_state state;

	/* HTTP_REQUEST parser */
	http_parser request_parser;
};

// Server socket file descriptor
static int listenfd;


// Epoll file descriptor
static int epollfd;

void handle_client(uint32_t event, struct connection *conn);
void handle_new_connection(void);
void handle_input(struct connection *conn);
void handle_output(struct connection *conn);

struct connection *connection_create(int sockfd);
void connection_remove(struct connection *conn);

int connection_open_file(struct connection *conn);

int connection_send_dynamic(struct connection *conn);
void connection_start_async_io(struct connection *conn);
enum connection_state connection_send_static(struct connection *conn);
void connection_complete_async_io(struct connection *conn);
void connection_start_async_io(struct connection *conn);

int parse_header(struct connection *conn);

void receive_data(struct connection *conn);

void handle_no_file(struct connection *conn);
void handle_static_file(struct connection *conn);
void handle_dynamic_file(struct connection *conn);

enum connection_state send_message(struct connection *conn);

#ifdef __cplusplus
}
#endif

#endif /* AWS_H_ */
