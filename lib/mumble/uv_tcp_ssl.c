#include <assert.h>
#include <stdio.h>

#include "uv_tcp_ssl.h"

typedef struct {
  uv_tcp_ssl_t *socket;
  mumble_uv_connect_cb cb;
} uv_ssl_connect_req_t;

typedef struct {
  uv_tcp_ssl_t *socket;
  char *buf;
} uv_ssl_write_t;

static void uv_ssl_handshake(uv_ssl_connect_req_t *creq);

static int uv_ssl_do_read(uv_tcp_ssl_t *socket) {
  int retry = 1;

  char *buf = buffer_pool_acquire(&socket->buffer_pool);
  assert(buf != NULL);

  int ret = SSL_read(socket->ssl, buf, socket->buffer_pool.size);
  if (ret > 0) {
    if (socket->cb != NULL) {
      socket->cb(socket, MUMBLE_CONNECTED, buf, ret);
    }
  } else if (ret == 0) {
    int r = SSL_get_error(socket->ssl, ret);
    fprintf(stderr, "Error when ret == 0: %d\n", r);
    retry = 0;
  } else {
    int r = SSL_get_error(socket->ssl, ret);
    switch (r) {
      case SSL_ERROR_WANT_READ:
        break;
      case SSL_ERROR_WANT_WRITE:
        break;
      default:
        fprintf(stderr, "Error when ret < 0: %d\n", r);
        break;
    }
    retry = 0;
  }

  buffer_pool_release(&socket->buffer_pool, buf);
  return retry;
}

static void uv_ssl_read(uv_tcp_ssl_t *socket) {
  while (uv_ssl_do_read(socket)) {
  }
}

/* Called after libuv receives data on the socket.
 * Either finishes the handshake or calls the user cb
 */
static long uv_ssl_rbio_cb(BIO *b, int oper, const char *argp, int argi, long argl, long retvalue) {
  uv_ssl_connect_req_t *creq = (uv_ssl_connect_req_t*)BIO_get_callback_arg(b);
  switch (oper) {
    case BIO_CB_WRITE|BIO_CB_RETURN:
      // Writing from network to libssl buffer
      if (SSL_in_init(creq->socket->ssl)) {
        // Called during ssl handshake
        uv_ssl_handshake(creq);
      } else if (SSL_is_init_finished(creq->socket->ssl)) {
        // Tell SSL we have more data to decode
        uv_ssl_read(creq->socket);
      } else {
        fprintf(stderr, "Reading from network when state unknown\n");
        abort();
      }
      break;
    case BIO_CB_READ:
      // Output to SSL
      break;
    default:
      break;
  }
  return retvalue;
}


/* Called when libssl wants to send data to the socket.
 */
static long uv_ssl_wbio_cb(BIO *b, int oper, const char *argp, int argi, long argl, long retvalue) {
  uv_tcp_ssl_t *socket = (uv_tcp_ssl_t*)BIO_get_callback_arg(b);
  switch (oper) {
    case BIO_CB_WRITE|BIO_CB_RETURN:
      // Writing from SSL to the network
      ;

      // Copy data from libssl buffer to a buffer usable by libuv
      char* arg = buffer_pool_acquire(&socket->buffer_pool);
      assert(arg != NULL);
      memcpy(arg, argp, argi);
      uv_buf_t buf = uv_buf_init(arg, argi);

      // Write to network
      int ret = uv_try_write(&socket->tcp, &buf, 1);
      assert(ret == argi);

      buffer_pool_release(&socket->buffer_pool, arg);
      break;
    case BIO_CB_READ:
      // Output to UV
      break;
    default:
      break;
  }

  return retvalue;
}

/* Initializes a socket with a libuv tcp handle and proper SSL objects
 */
void mumble_uv_ssl_init(uv_tcp_ssl_t *socket, uv_loop_t *loop) {
  memset(socket, 0, sizeof(uv_tcp_ssl_t));

  // Create the libuv socket
  int ret = uv_tcp_init(loop, &socket->tcp);
  socket->tcp.data = socket;
  assert(ret == 0);

  // Create a new SSL context
  SSL_CTX *ssl_ctx = SSL_CTX_new(SSLv23_client_method());
  assert(ssl_ctx != NULL);

  ret = SSL_CTX_set_default_verify_paths(ssl_ctx);
  assert(ret == 1);
  SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_SSLv3);
  // SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);

  socket->ssl = SSL_new(ssl_ctx);
  SSL_CTX_free(ssl_ctx);
  assert(socket->ssl != NULL);

  BIO *rbio = BIO_new(BIO_s_mem());
  BIO_set_callback(rbio, uv_ssl_rbio_cb);
  BIO_set_callback_arg(rbio, NULL);

  BIO *wbio = BIO_new(BIO_s_null());
  BIO_set_callback(wbio, uv_ssl_wbio_cb);
  BIO_set_callback_arg(wbio, (char*)socket);

  SSL_set_bio(socket->ssl, rbio, wbio);
  SSL_set_connect_state(socket->ssl);

  buffer_pool_init(&socket->buffer_pool, 4, 16*1024); // 4 buffers of 16kB
}

void mumble_uv_ssl_close(uv_tcp_ssl_t *socket) {
  SSL_shutdown(socket->ssl);
}

void mumble_uv_ssl_free(uv_tcp_ssl_t *socket) {
  SSL_free(socket->ssl);
}

void mumble_uv_ssl_peername(const uv_tcp_ssl_t *socket, struct sockaddr *name, int *namelen) {
  int ret = uv_tcp_getpeername(&socket->tcp, name, namelen);
  assert(ret == 0);
}

static void default_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  uv_tcp_ssl_t *socket = handle->data;
  buf->base = buffer_pool_acquire(&socket->buffer_pool);
  assert(buf->base != NULL);
  buf->len = socket->buffer_pool.size;
}

static void uv_ssl_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  if (nread > 0) {
    uv_tcp_ssl_t *socket = (uv_tcp_ssl_t*)stream->data;
    BIO *rbio = SSL_get_rbio(socket->ssl);
    assert(rbio != NULL);
    int ret = BIO_write(rbio, buf->base, nread);
    assert(ret == nread);
    buffer_pool_release(&socket->buffer_pool, buf->base);
  }
}

static void uv_ssl_handshake(uv_ssl_connect_req_t *creq) {
  int ret = SSL_do_handshake(creq->socket->ssl);
  int req = SSL_get_error(creq->socket->ssl, ret);
  switch (req) {
    case 0:
      if (creq->cb != NULL) {
        creq->cb(creq->socket, MUMBLE_CONNECTED);
      }
      break;
    case 1:
      ;
      long result = SSL_get_verify_result(creq->socket->ssl);
      if (result != X509_V_OK) {
        fprintf(stderr, "Invalid cert: %s\n", X509_verify_cert_error_string(result));
      }
      if (creq->cb != NULL) {
        creq->cb(creq->socket, MUMBLE_FAILED);
      }
    default:
      // fprintf(stderr, "SSL State: %d / %d\n", ret, req);
      // fprintf(stderr, "SSL State: %s\n", SSL_state_string_long(creq->socket->ssl));
      break;
  }
}

static void uv_ssl_connect_cb(uv_connect_t* req, int status) {
  assert(status == 0);
  
  uv_ssl_connect_req_t* creq = (uv_ssl_connect_req_t*)req->data;
  int ret = uv_read_start((uv_stream_t*)&creq->socket->tcp, default_alloc_cb, uv_ssl_read_cb);
  assert (ret == 0);

  uv_ssl_handshake(creq);
}

static void uv_ssl_dns_cb(uv_getaddrinfo_t* req_dns, int status, struct addrinfo* res) {
  assert(status == 0);
  fprintf(stderr, "Connecting\n");

  uv_connect_t req;
  req.data = req_dns->data;
  uv_ssl_connect_req_t* creq = (uv_ssl_connect_req_t*)req.data;

  int ret = uv_tcp_connect(&req, &creq->socket->tcp, res->ai_addr, uv_ssl_connect_cb);
  assert(ret == 0);

  uv_freeaddrinfo(res);
  free(req_dns);
}

void mumble_uv_ssl_connect(uv_tcp_ssl_t *socket, const char* hostname, const char* port, mumble_uv_connect_cb cb) {
  fprintf(stderr, "Looking up %s\n", hostname);

  uv_ssl_connect_req_t *creq = malloc(sizeof(uv_ssl_connect_req_t));
  assert(creq != NULL);

  creq->socket = socket;
  creq->cb = cb;

  uv_getaddrinfo_t *req = (uv_getaddrinfo_t*)malloc(sizeof(uv_getaddrinfo_t));
  req->data = creq;

  struct addrinfo hints;
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = 0;

  BIO *rbio = SSL_get_rbio(creq->socket->ssl);
  assert(rbio != NULL);
  BIO_set_callback_arg(rbio, (char*)creq);

  int ret = uv_getaddrinfo(socket->tcp.loop, req, uv_ssl_dns_cb, hostname, port, &hints);
  assert(ret == 0);
}

void mumble_uv_ssl_set_data(uv_tcp_ssl_t *socket, void *data) {
  socket->data = data;
}

void mumble_uv_ssl_set_cb(uv_tcp_ssl_t *socket, mumble_uv_read_cb cb) {
  socket->cb = cb;
}

int mumble_uv_ssl_write(const uv_tcp_ssl_t *socket, const void* buf, int size) {
  int ret = SSL_write(socket->ssl, buf, size);
  assert(ret > 1);
}
