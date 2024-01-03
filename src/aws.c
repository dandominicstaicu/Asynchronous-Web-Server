// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, ".", 2);
	strncat(conn->request_path, buf, len);
	conn->request_path[len + 1] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* Prepare the connection buffer to send the reply header. */
	struct stat file_stat;

	fstat(conn->fd, &file_stat);

	conn->file_pos = 0;
	conn->file_size = file_stat.st_size;
	conn->send_len = sprintf(conn->send_buffer, SUCCESS_MSG, conn->file_size);
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* Prepare the connection buffer to send the 404 header. */
	strncat(conn->send_buffer, ERROR_MSG, BUFSIZ);
	conn->send_len = sizeof(ERROR_MSG);
	conn->fd = -1;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */

	int static_file = strncmp(conn->request_path, AWS_ABS_STATIC_FOLDER, strlen(AWS_ABS_STATIC_FOLDER));
	int dynamic_file = strncmp(conn->request_path, AWS_ABS_DYNAMIC_FOLDER, strlen(AWS_ABS_DYNAMIC_FOLDER));

	if (static_file == 0)
		return RESOURCE_TYPE_STATIC;
	else if (dynamic_file == 0)
		return RESOURCE_TYPE_DYNAMIC;

	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	/* Initialize connection structure on given socket. */
	struct connection *conn = calloc(1, sizeof(*conn));

	DIE(conn == NULL, "calloc failed err");

	conn->sockfd = sockfd;

	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	memset(conn->request_path, 0, BUFSIZ);

	return conn;
}

void update_connection_state_for_sending(struct connection *conn, struct io_event *event)
{
	conn->file_pos += event->res;
	io_prep_pwrite(&conn->iocb, conn->sockfd, conn->send_buffer, event->res, 0);
	*conn->piocb = &conn->iocb;
	io_set_eventfd(&conn->iocb, conn->eventfd);
	conn->state = STATE_SENDING_DATA;
}

int check_and_update_connection_state(struct connection *conn)
{
	if (conn->file_pos == conn->file_size) {
		conn->state = STATE_DATA_SENT;
		int remove_result = w_epoll_remove_ptr(epollfd, conn->eventfd, conn);

		DIE(remove_result < 0, "w_epoll_remove_ptr");

		int add_result = w_epoll_add_ptr_in(epollfd, conn->sockfd, conn);

		DIE(add_result < 0, "w_epoll_add_in");

		clean_up_connection(conn);

		return -1;
	}

	connection_complete_async_io(conn);
	conn->state = STATE_ASYNC_ONGOING;

	return 0;
}

void clean_up_connection(struct connection *conn)
{
	io_destroy(conn->ctx);
	close(conn->eventfd);

	free(conn->piocb);
}

void connection_start_async_io(struct connection *conn)
{
	/* Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	struct io_event event;
	int rc = io_getevents(conn->ctx, 1, 1, &event, NULL);

	DIE(rc < 0, "io_getevents");

	switch (conn->state) {
	case STATE_ASYNC_ONGOING:
		update_connection_state_for_sending(conn, &event);
		break;

	case STATE_SENDING_DATA:
		rc = check_and_update_connection_state(conn);
		if (rc == -1)
			return;
		break;
	}

	rc = io_submit(conn->ctx, 1, conn->piocb);
	DIE(rc < 0, "io_submit error");
}

void connection_remove(struct connection *conn)
{
	/* Remove connection handler. */
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

void handle_new_connection(void)
{
	/* Handle a new connection request on the server socket. */
	int newfd;
	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);
	struct connection *conn;
	int flags;
	int rc = 0;

	/* Accept new connection. */
	newfd = accept(listenfd, (SSA *) &client_addr, &client_len);
	DIE(newfd < 0, "accept");

	/* Set socket to be non-blocking. */
	flags = fcntl(newfd, F_GETFL, 0);
	DIE(flags < 0, "fcntl F_GETFL");
	dlog(LOG_INFO, "Accepted connection from: %s:%d\n",
		 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));


	flags = (flags | O_NONBLOCK);
	rc = fcntl(newfd, F_SETFL, flags);
	if (rc < 0) {
		perror("fcntl F_SETFL");
		close(newfd);
		return;
	}

	/* Instantiate new connection handler. */
	conn = connection_create(newfd);
	if (conn == NULL) {
		perror("Cannot create new connection\n");
		close(newfd);
		return;
	}

	/* Add socket to epoll. */
	rc = w_epoll_add_ptr_in(epollfd, newfd, conn);
	if (rc < 0) {
		perror("w_epoll_add_fd");
		connection_remove(conn);
		return;
	}
}

int is_end_of_header(struct connection *conn)
{
	const char *end_of_header = "\r\n\r\n";
	const size_t len = 4;

	if (conn->recv_len >= len)
		return strncmp(conn->recv_buffer + conn->recv_len - len, end_of_header, len);

	return 0;
}

void receive_data(struct connection *conn)
{
	/* Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	ssize_t recv_bytes = 0;
	int rc = 0;
	char abuffer[64] = {0};

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");

		connection_remove(conn);
		conn->state = STATE_CONNECTION_CLOSED;

		return;
	}

	recv_bytes = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
					  BUFSIZ - conn->recv_len, 0);

	dlog(LOG_DEBUG, "Received %ld bytes\n", recv_bytes);

	if (recv_bytes < 0) {
		dlog(LOG_ERR, "Error in comm from: %s\n", abuffer);

		rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_remove_ptr");

		connection_remove(conn);
		conn->state = STATE_CONNECTION_CLOSED;

		return;
	}

	if (recv_bytes == 0) {
		dlog(LOG_INFO, "recv Connection closed from: %s\n", abuffer);
		rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_remove_ptr");

		connection_remove(conn);
		conn->state = STATE_CONNECTION_CLOSED;

		return;
	}

	conn->recv_len += recv_bytes;
	if (is_end_of_header(conn)) {
		conn->state = STATE_RECEIVING_DATA;
		return;
	}

	dlog(LOG_DEBUG, "Received message from: %s\n", abuffer);

	printf("--\n%s--\n", conn->recv_buffer);

	conn->state = STATE_REQUEST_RECEIVED;
}

int connection_open_file(struct connection *conn)
{
	/* Open file and update connection fields. */
	dlog(LOG_DEBUG, "%s is of type %d\n", conn->request_path, conn->res_type);

	conn->fd = open(conn->request_path, O_RDONLY);
	if (conn->fd == -1) {
		dlog(LOG_ERR, "Error in open: %s\n", strerror(errno));
		connection_prepare_send_404(conn);
		return -1;
	}

	dlog(LOG_INFO, "conn->fd in parse header = %d\n", conn->fd);
	dlog(LOG_INFO, "conn->request_path in parse header = %s\n", conn->request_path);

	connection_prepare_send_reply_header(conn);

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	/* Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 * Prepare for reading more data
	 */
	int dif = conn->file_size - conn->file_pos;

	if (dif < BUFSIZ)
		io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, dif, conn->file_pos);
	else
		io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, BUFSIZ, conn->file_pos);

	*conn->piocb = &conn->iocb;
	io_set_eventfd(&conn->iocb, conn->eventfd);
}

int parse_header(struct connection *conn)
{
	/* Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	http_parser h_parser;

	http_parser_init(&h_parser, HTTP_REQUEST);

	size_t parsed_bytes = http_parser_execute(&h_parser,
		&settings_on_path, conn->recv_buffer, conn->recv_len);

	dlog(LOG_INFO, "Parsed HTTP request (bytes: %lu), path: %s\n", parsed_bytes, conn->request_path);

	dlog(LOG_DEBUG, "request_path: %s\n", conn->request_path);

	conn->res_type = connection_get_resource_type(conn);

	switch (conn->res_type) {
	case RESOURCE_TYPE_STATIC:
		dlog(LOG_DEBUG, "Sending static file\n");
		connection_open_file(conn);
		break;
	case RESOURCE_TYPE_DYNAMIC:
		connection_open_file(conn);
		break;
	case RESOURCE_TYPE_NONE:
		connection_prepare_send_404(conn);
		break;
	default:
		dlog(LOG_ERR, "Unknown resource type\n");
		break;
	}

	return parsed_bytes;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* Send static data using sendfile(2). */
	conn->state = STATE_ASYNC_ONGOING;

	int nr_bytes = conn->file_size - conn->file_pos;

	if (nr_bytes > BUFSIZ)
		nr_bytes = BUFSIZ;

	dlog(LOG_DEBUG, "conn->fd = %d\n", conn->fd);

	ssize_t sent_bytes = sendfile(conn->sockfd, conn->fd, &conn->file_pos,
						nr_bytes);

	if (sent_bytes == -1) {
		dlog(LOG_ERR, "Error in sendfile: %s\n", strerror(errno));
		connection_remove(conn);
		return STATE_CONNECTION_CLOSED;
	}

	if (conn->file_size == conn->file_pos) {
		close(conn->fd);
		conn->state = STATE_DATA_SENT;
		return STATE_DATA_SENT;
	}

	return STATE_ASYNC_ONGOING;
}

enum connection_state connection_send_data(struct connection *conn)
{
	/* Send as much data as possible from the connection send buffer.
	 * Returns the state of the connection.
	 */
	ssize_t sent_bytes = 0;
	int rc = 0;
	char abuffer[64] = {0};

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_remove_ptr");

		connection_remove(conn);
		conn->state = STATE_CONNECTION_CLOSED;

		return STATE_CONNECTION_CLOSED;
	}

	sent_bytes = send(conn->sockfd, conn->send_buffer + conn->send_pos,
					  conn->send_len, 0);
	if (sent_bytes < 0) {
		dlog(LOG_ERR, "Error in comm from: %s\n", abuffer);
		rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_remove_ptr");

		connection_remove(conn);
		conn->state = STATE_CONNECTION_CLOSED;

		return STATE_CONNECTION_CLOSED;
	}

	if (sent_bytes == 0) {
		dlog(LOG_INFO, "send Connection closed from: %s\n", abuffer);

		rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_remove_ptr");

		connection_remove(conn);
		conn->state = STATE_CONNECTION_CLOSED;
		return STATE_CONNECTION_CLOSED;
	}

	conn->send_pos += sent_bytes;
	conn->send_len -= sent_bytes;

	if (conn->send_len > 0) {
		conn->state = STATE_SENDING_HEADER;
		return STATE_SENDING_HEADER;
	}

	dlog(LOG_INFO, "Sending message to: %s ->\n\n%s\n", abuffer, conn->send_buffer);

	conn->state = STATE_HEADER_SENT;
	return STATE_HEADER_SENT;
}

void setup_io_context(struct connection *conn)
{
	int setup_result = io_setup(1, &conn->ctx);

	DIE(setup_result < 0, "io_setup");
}

void prepare_dynamic_io(struct connection *conn)
{
	int remaining_bytes = conn->file_size - conn->file_pos;
	int read_bytes = remaining_bytes > BUFSIZ ? BUFSIZ : remaining_bytes;

	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, read_bytes, conn->file_pos);
	*conn->piocb = &conn->iocb;
	io_set_eventfd(&conn->iocb, conn->eventfd);
}

void submit_io_request(struct connection *conn)
{
	int submit_result = io_submit(conn->ctx, 1, conn->piocb);

	DIE(submit_result < 0, "io_submit");
}

void connection_send_dynamic(struct connection *conn)
{
	int res = 0;

	conn->state = STATE_ASYNC_ONGOING;

	res = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(res < 0, "w_epoll_remove_ptr");

	conn->piocb[0] = &conn->iocb;

	conn->eventfd = eventfd(0, 0);
	DIE(conn->eventfd < 0, "eventfd");

	setup_io_context(conn);

	res = w_epoll_add_ptr_in(epollfd, conn->eventfd, conn);
	DIE(res < 0, "w_epoll_add_ptr_in");

	prepare_dynamic_io(conn);
	submit_io_request(conn);
}


void handle_input(struct connection *conn)
{
	/* Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	int rc = 0;

	receive_data(conn);
	if (conn->state != STATE_REQUEST_RECEIVED)
		return;

	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_out");

	parse_header(conn);
}

void handle_no_file(struct connection *conn)
{
	int rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);

	DIE(rc < 0, "w_epoll_update_ptr_in");
}

void handle_static_file(struct connection *conn)
{
	if (connection_send_static(conn) != STATE_DATA_SENT) {
		dlog(LOG_DEBUG, "conn->state = %d\n", conn->state);
		return;
	}

	dlog(LOG_DEBUG, "conn->state = %d\n", conn->state);

	int rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);

	DIE(rc < 0, "w_epoll_update_ptr_in");
}

void handle_output(struct connection *conn)
{
	if ((conn->state == STATE_REQUEST_RECEIVED || conn->state == STATE_SENDING_HEADER) &&
		(connection_send_data(conn) != STATE_HEADER_SENT)) {
		dlog(LOG_DEBUG, "Not all data sent\n");
		return;
	}

	switch (conn->res_type) {
	case RESOURCE_TYPE_NONE:
		dlog(LOG_DEBUG, "RESOURCE_TYPE_NONE\n");
		handle_no_file(conn);
		break;
	case RESOURCE_TYPE_STATIC:
		dlog(LOG_DEBUG, "RESOURCE_TYPE_STATIC\n");
		handle_static_file(conn);
		break;
	case RESOURCE_TYPE_DYNAMIC:
		dlog(LOG_DEBUG, "RESOURCE_TYPE_DYNAMIC\n");
		connection_send_dynamic(conn);
		break;
	default:
		dlog(LOG_ERR, "Unknown resource type\n");
		break;
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
	if ((event & EPOLLIN)) {
		dlog(LOG_DEBUG, "New message\n");
		handle_input(conn);
	} else if ((event & EPOLLOUT)) {
		dlog(LOG_DEBUG, "Ready to send message\n");
		handle_output(conn);
	}
}

int main(void)
{
	int rc;

	/* Initialize asynchronous operations. */
	rc = io_setup(MAX_EVENTS, &ctx);
	DIE(rc < 0, "io_setup");

	/* Initialize multiplexing. */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* Create server socket. */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
		DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	/* Add server socket to epoll object*/
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* Uncomment the following line for debugging. */
	dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* Wait for events. */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/* Switch event types; considering:
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */

		if (rev.data.fd == listenfd) {
			dlog(LOG_INFO, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			struct connection *conn = rev.data.ptr;

			if (conn->res_type == RESOURCE_TYPE_DYNAMIC &&
			(conn->state == STATE_ASYNC_ONGOING || conn->state == STATE_SENDING_DATA)) {
				dlog(LOG_DEBUG, "async file send\n");
				connection_start_async_io(conn);
			} else {
				handle_client(rev.events, conn);
			}
		}
	}

	return 0;
}
