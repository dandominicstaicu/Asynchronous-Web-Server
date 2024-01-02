// void connection_start_async_io(struct connection *conn)
// {
// 	/* Start asynchronous operation (read from file).
// 	 * Use io_submit(2) & friends for reading data asynchronously.
// 	 */

// 	int rc;
// 	struct io_event event;

// 	rc = io_getevents(conn->ctx, 1, 1, &event, NULL);
// 	DIE(rc < 0, "io_getevents");

// 	switch(conn->state) {
// 		case STATE_ASYNC_ONGOING:
// 			/* Increase the file position by the number of bytes read. */
// 			conn->file_pos += event.res;

// 			/* Prepare for sending the data. */
// 			io_prep_pwrite(conn->iocb, conn->sockfd, conn->send_buffer, event.res, 0);
// 			*conn->piocb = conn->iocb;
// 			io_set_eventfd(conn->iocb, conn->eventfd);

// 			/* Update the state. */
// 			conn->state = STATE_SENDING_DATA;
// 			break;

// 		case STATE_SENDING_DATA:
// 			/* Check if all the data has been sent. */
// 			if (conn->file_pos == conn->file_size) {
// 				conn->state = STATE_DATA_SENT;

// 				/* Remove the event from epoll. */
// 				rc = w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
// 				DIE(rc < 0, "w_epoll_remove_ptr");

// 				/* Add socket to epoll. */
// 				rc = w_epoll_add_ptr_in(epollfd, conn->sockfd, conn);
// 				DIE(rc < 0, "w_epoll_add_in");

// 				/* Clean it all up. */
// 				io_destroy(conn->ctx);
// 				close(conn->eventfd);

// 				free(conn->iocb);
// 				free(conn->piocb);
// 				return;
// 			}

// 			connection_complete_async_io(conn);

// 			/* Update the state. */
// 			conn->state = STATE_ASYNC_ONGOING;
// 			break;
// 	}

// 	/* Submit the request */
// 	rc = io_submit(conn->ctx, 1, conn->piocb);
// 	DIE(rc < 0, "io_submit error");
// }