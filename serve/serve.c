#include <ccv.h>
#include <ev.h>
#include <dispatch/dispatch.h>
#include "ebb.h"
#include "uri.h"
#include "async.h"

static const char ebb_http_404[] = "HTTP/1.1 404 Not Found\r\nCache-Control: no-cache\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: 6\r\n\r\nfalse\n";

typedef struct {
	ebb_request* request;
} ebb_connection_extras;

typedef struct {
	ebb_connection* connection;
	uri_dispatch_t* dispatcher;
	int recv_multipart;
	int multipart_bm_pattern;
	int multipart_boundary_delta1[255];
	int multipart_boundary_delta2[EBB_MAX_MULTIPART_BOUNDARY_LEN];
	void* context;
	ebb_buf response;
} ebb_request_extras;

static void on_request_path(ebb_request* request, const char* at, size_t length)
{
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	char* path = (char*)at;
	char eof = path[length];
	path[length] = '\0';
	request_extras->dispatcher = find_uri_dispatch(path);
	request_extras->context = 0;
	path[length] = eof;
}

static void on_request_query_string(ebb_request* request, const char* at, size_t length)
{
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	if (request_extras->dispatcher && request_extras->dispatcher->parse)
		request_extras->context = request_extras->dispatcher->parse(request_extras->dispatcher->context, request_extras->context, at, length, URI_QUERY_STRING, 0);
}

static void on_request_part_data(ebb_request* request, const char* at, size_t length)
{
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	if (request_extras->dispatcher && request_extras->dispatcher->parse)
		request_extras->context = request_extras->dispatcher->parse(request_extras->dispatcher->context, request_extras->context, at, length, URI_MULTIPART_DATA, -1);
}

static void on_request_multipart_header_field(ebb_request* request, const char* at, size_t length, int header_index)
{
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	if (request_extras->dispatcher && request_extras->dispatcher->parse)
		request_extras->context = request_extras->dispatcher->parse(request_extras->dispatcher->context, request_extras->context, at, length, URI_MULTIPART_HEADER_FIELD, header_index);
}

static void on_request_multipart_header_value(ebb_request* request, const char* at, size_t length, int header_index)
{
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	if (request_extras->dispatcher && request_extras->dispatcher->parse)
		request_extras->context = request_extras->dispatcher->parse(request_extras->dispatcher->context, request_extras->context, at, length, URI_MULTIPART_HEADER_VALUE, header_index);
}

static void on_request_body(ebb_request* request, const char* at, size_t length)
{
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	if (request_extras->dispatcher && request_extras->dispatcher->parse && request->multipart_boundary_len == 0)
	{
		char* body = (char*)at;
		char eof = body[length];
		body[length] = '\0';
		//request_extras->context = request_extras->dispatcher->parse(request_extras->context, 0, body);
		body[length] = eof;
	}
}

static void on_connection_response_continue(ebb_connection* connection)
{
	ebb_connection_extras* connection_extras = (ebb_connection_extras*)(connection->data);
	ebb_request* request = connection_extras->request;
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	// call custom release function for the buffer
	if (request_extras->response.data && request_extras->response.on_release)
		request_extras->response.on_release(&request_extras->response);
	ebb_connection_schedule_close(connection);
}

static void on_request_response(void* context)
{
	ebb_request* request = (ebb_request*)context;
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	ebb_connection* connection = request_extras->connection;
	ebb_connection_write(connection, request_extras->response.data, request_extras->response.len, on_connection_response_continue);
	ccfree(request);
}

static void on_request_execute(void* context)
{
	// this is called off-thread
	ebb_request* request = (ebb_request*)context;
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	const static char http_bad_request[] =
		"HTTP/1.1 400 Bad Request\r\nCache-Control: no-cache\r\nContent-Type: application/json; charset=utf8\r\nContent-Length: 6\r\n\r\n"
		"false\n";
	int response_code = 0;
	request_extras->response.on_release = 0;
	switch (request->method)
	{
		case EBB_POST:
			if (request_extras->dispatcher->post)
			{
				response_code = request_extras->dispatcher->post(request_extras->dispatcher->context, request_extras->context, &request_extras->response);
				break;
			}
		case EBB_GET:
			if (request_extras->dispatcher->get)
			{
				response_code = request_extras->dispatcher->get(request_extras->dispatcher->context, request_extras->context, &request_extras->response);
				break;
			}
		default:
			request_extras->response.data = (void*)ebb_http_404;
			request_extras->response.len = sizeof(ebb_http_404);
			break;
	}
	if (response_code != 0)
	{
		assert(request_extras->response.on_release == 0);
		request_extras->response.data = (void*)http_bad_request;
		request_extras->response.len = sizeof(http_bad_request);
	}
	main_async_f(request, on_request_response);
}

static void on_request_dispatch(ebb_request* request)
{
	ebb_request_extras* request_extras = (ebb_request_extras*)request->data;
	ebb_connection* connection = request_extras->connection;
	if (request_extras->dispatcher)
		dispatch_async_f(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), request, on_request_execute);
	else // write 404
		ebb_connection_write(connection, ebb_http_404, sizeof(ebb_http_404), on_connection_response_continue);
}

static ebb_request* new_request(ebb_connection* connection)
{
	ebb_request* request = (ebb_request*)ccmalloc(sizeof(ebb_request) + sizeof(ebb_request_extras));
	ebb_request_init(request);
	ebb_connection_extras* connection_extras = (ebb_connection_extras*)(connection->data);
	connection_extras->request = request;
	ebb_request_extras* request_extras = (ebb_request_extras*)(request + 1);
	request_extras->connection = connection;
	request_extras->dispatcher = 0;
	request_extras->recv_multipart = 0;
	request_extras->multipart_bm_pattern = 0;
	request->data = request_extras;
	request->on_path = on_request_path;
	request->on_part_data = on_request_part_data;
	request->on_multipart_header_field = on_request_multipart_header_field;
	request->on_multipart_header_value = on_request_multipart_header_value;
	request->on_body = on_request_body;
	request->on_query_string = on_request_query_string;
	request->on_complete = on_request_dispatch;
	return request;
}

static void on_connection_close(ebb_connection* connection)
{
	ccfree(connection);
}

static ebb_connection* new_connection(ebb_server* server, struct sockaddr_in* addr)
{
	ebb_connection* connection = (ebb_connection*)ccmalloc(sizeof(ebb_connection) + sizeof(ebb_connection_extras));
	ebb_connection_init(connection);
	ebb_connection_extras* connection_extras = (ebb_connection_extras*)(connection + 1);
	connection_extras->request = 0;
	connection->data = connection_extras;
	connection->new_request = new_request;
	connection->on_close = on_connection_close;
	return connection;
}

int main(int argc, char** argv)
{
	ebb_server server;
	ebb_server_init(&server, EV_DEFAULT);
	server.new_connection = new_connection;
	ebb_server_listen_on_port(&server, 3350);
	uri_init();
	main_async_init();
	main_async_start(EV_DEFAULT);
	printf("listen on 3350, http://localhost:3350/\n");
	ev_run(EV_DEFAULT_ 0);
	main_async_destroy();
	return 0;
}
