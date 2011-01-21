#include "evn.h"

#include <errno.h> // errno
#include <unistd.h> // close
#include <stdlib.h> // free
#include <string.h> // memcpy

#define EVN_MAX_RECV 4096
static int evn_server_unix_create(struct sockaddr_un* socket_un, char* sock_path);
static char recv_data[EVN_MAX_RECV];

inline struct evn_stream* evn_stream_create(int fd) {
  evn_debug("evn_stream_create");
  struct evn_stream* stream;

  //stream = realloc(NULL, sizeof(struct evn_stream));
  stream = calloc(1, sizeof(struct evn_stream));
  stream->fd = fd;
  // stream->type = 0;
  // stream->oneshot = false;
  evn_set_nonblock(stream->fd);
  ev_io_init(&stream->io, evn_stream_read_priv_cb, stream->fd, EV_READ);

  evn_debugs(".");
  return stream;
}

int evn_stream_destroy(EV_P_ struct evn_stream* stream)
{
  // TODO delay freeing of server until streams have closed
  // or link loop directly to stream?
  ev_io_stop(EV_A_ &stream->io);
  close(stream->fd);
  free(stream);
  return 0;
}

// This callback is called when data is readable on the unix socket.
void evn_stream_read_priv_cb(EV_P_ ev_io *w, int revents)
{
  void* data;
  struct evn_exception error;
  int length;
  struct evn_stream* stream = (struct evn_stream*) w;

  evn_debugs("EV_READ - stream->fd");
  usleep(100);
  length = recv(stream->fd, &recv_data, 4096, 0);

  if (length < 0)
  {
    if (stream->error) { stream->error(EV_A_ stream, &error); }
  }
  else if (0 == length)
  {
    if (stream->oneshot)
    {
      // TODO put on stack char data[stream->bufferlist->used];
      evn_buffer* buffer = evn_bufferlist_concat(stream->bufferlist);
      if (stream->data) { stream->data(EV_A_ stream, buffer->data, buffer->used); }
      free(buffer); // does not free buffer->data, that's up to the user
    }
    if (stream->close) { stream->close(EV_A_ stream, false); }
    evn_stream_destroy(EV_A_ stream);
  }
  else if (length > 0)
  {
    if (stream->oneshot)
    {
      // if (stream->progress) { stream->progress(EV_A_ stream, data, length); }
      // if time - stream->started_at > stream->max_wait; stream->timeout();
      // if buffer->used > stream->max_size; stream->timeout();
      evn_bufferlist_add(stream->bufferlist, &recv_data, length);
      return;
    }
    data = malloc(length);
    memcpy(data, &recv_data, length);
    if (stream->data) { stream->data(EV_A_ stream, recv_data, length); }
  }
}

void evn_server_connection_priv_cb(EV_P_ ev_io *w, int revents)
{
  evn_debugs("new connection - EV_READ - server->fd has become readable");

  int stream_fd;
  struct evn_stream* stream;

  // since ev_io is the first member,
  // watcher `w` has the address of the
  // start of the evn_server struct
  struct evn_server* server = (struct evn_server*) w;

  while (1)
  {
    stream_fd = accept(server->fd, NULL, NULL);
    if (stream_fd == -1)
    {
      if( errno != EAGAIN && errno != EWOULDBLOCK )
      {
        fprintf(stderr, "accept() failed errno=%i (%s)", errno, strerror(errno));
        exit(EXIT_FAILURE);
      }
      break;
    }
    stream = evn_stream_create(stream_fd);
    if (server->connection) { server->connection(EV_A_ server, stream); }
    if (stream->oneshot)
    {
      // each buffer chunk should be at least 4096 and we'll start with 128 chunks
      // the size will grow if the actual data received is larger
      stream->bufferlist = evn_bufferlist_create(4096, 128);
    }
    //stream->index = array_push(&server->streams, stream);
    ev_io_start(EV_A_ &stream->io);
  }
  evn_debugs(".");
}

// Simply adds O_NONBLOCK to the file descriptor of choice
int evn_set_nonblock(int fd)
{
  int flags;

  flags = fcntl(fd, F_GETFL);
  flags |= O_NONBLOCK;
  return fcntl(fd, F_SETFL, flags);
}

static int evn_server_unix_create(struct sockaddr_un* socket_un, char* sock_path)
{
  int fd;

  unlink(sock_path);

  // Setup a unix socket listener.
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (-1 == fd) {
    perror("echo server socket");
    exit(EXIT_FAILURE);
  }

  // Set it non-blocking
  if (-1 == evn_set_nonblock(fd)) {
    perror("echo server socket nonblock");
    exit(EXIT_FAILURE);
  }

  // Set it as unix socket
  socket_un->sun_family = AF_UNIX;
  strcpy(socket_un->sun_path, sock_path);

  return fd;
}

int evn_server_destroy(EV_P_ struct evn_server* server)
{
  if (server->close) { server->close(server->EV_A_ server); }
  ev_io_stop(server->EV_A_ &server->io);
  free(server);
  return 0;
}

struct evn_server* evn_server_create(EV_P_ char* sock_path, int max_queue)
{
    struct evn_server* server = calloc(1, sizeof(struct evn_server));

    server->EV_A = EV_A;
    server->fd = evn_server_unix_create(&server->socket, sock_path);
    server->socket_len = sizeof(server->socket.sun_family) + strlen(server->socket.sun_path);

    //array_init(&server->streams, 128);

    if (-1 == bind(server->fd, (struct sockaddr*) &server->socket, server->socket_len))
    {
      perror("echo server bind");
      exit(EXIT_FAILURE);
    }

    if (-1 == listen(server->fd, max_queue)) {
      perror("listen");
      exit(EXIT_FAILURE);
    }
    
    return server;
}

int evn_server_listen(struct evn_server* server)
{
  ev_io_init(&server->io, evn_server_connection_priv_cb, server->fd, EV_READ);
  ev_io_start(server->EV_A_ &server->io);

  return 0;
}
