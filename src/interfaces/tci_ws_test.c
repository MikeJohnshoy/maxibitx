// tci_ws_test.c
//
// Bench harness for tci_ws.c over loopback, no radio: the RFC 6455 accept
// vector, the handshake (and its refusals), masked and unmasked frames, all
// three length encodings, a fragmented message with a ping inside it, close
// from either side, oversize and malformed frames, the client limit, ids
// that a later client in the same slot doesn't inherit, and the send
// queue's drop policy against a client that doesn't read.
// Build and run: make test-tci-ws

#define _GNU_SOURCE
#include "tci_ws.h"
#include "ws_test_client.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...)                                                                           \
  do {                                                                                             \
    if (cond) {                                                                                    \
      printf("  ok   ");                                                                           \
    } else {                                                                                       \
      printf("  FAIL ");                                                                           \
      failures++;                                                                                  \
    }                                                                                              \
    printf(__VA_ARGS__);                                                                           \
    printf("\n");                                                                                  \
  } while (0)

/* ---- Server side: callbacks that echo, and a record of events ---------- */

static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static int opens, closes, last_open_id = -1, last_close_id = -1;
static char last_text[512];

static void on_open(int id, const char *peer) {
  (void)peer;
  pthread_mutex_lock(&ev_lock);
  opens++;
  last_open_id = id;
  pthread_mutex_unlock(&ev_lock);
}

static void on_text(int id, const char *msg, size_t len) {
  pthread_mutex_lock(&ev_lock);
  snprintf(last_text, sizeof(last_text), "%.*s", (int)(len < 500 ? len : 500), msg);
  pthread_mutex_unlock(&ev_lock);
  if (!strcmp(msg, "please close"))
    ws_close_client(id);
  else
    ws_send_text(id, msg);
}

static void on_binary(int id, const uint8_t *data, size_t len) { ws_send_binary(id, data, len); }

static void on_close(int id) {
  pthread_mutex_lock(&ev_lock);
  closes++;
  last_close_id = id;
  pthread_mutex_unlock(&ev_lock);
}

static int event_count(int *which) {
  pthread_mutex_lock(&ev_lock);
  int v = *which;
  pthread_mutex_unlock(&ev_lock);
  return v;
}

static int wait_for(int *which, int value) {
  for (int i = 0; i < 200; i++) {
    if (event_count(which) >= value)
      return 1;
    usleep(5000);
  }
  return 0;
}

static int expect_close(int fd, int code) {
  uint8_t *p = NULL;
  size_t n;
  int op = recv_frame(fd, &p, &n, NULL);
  int got = (op == 8 && n >= 2) ? (p[0] << 8 | p[1]) : -1;
  free(p);
  char c;
  int eof = recv(fd, &c, 1, 0) <= 0;
  return got == code && eof;
}

/* ---- Tests ------------------------------------------------------------ */

static void test_accept_vector(void) {
  char out[29];
  ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", out);
  CHECK(!strcmp(out, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), "RFC 6455 accept vector: %s", out);
}

static void test_handshake_refusals(void) {
  int fd = tcp_connect();
  const char *nokey = "GET / HTTP/1.1\r\nUpgrade: websocket\r\n\r\n";
  send(fd, nokey, strlen(nokey), 0);
  char head[512];
  read_http(fd, head, sizeof(head));
  CHECK(strstr(head, " 400 ") != NULL, "request without Sec-WebSocket-Key refused with 400");
  close(fd);

  fd = tcp_connect();
  const char *post = "POST / HTTP/1.1\r\nUpgrade: websocket\r\nSec-WebSocket-Key: x\r\n\r\n";
  send(fd, post, strlen(post), 0);
  read_http(fd, head, sizeof(head));
  CHECK(strstr(head, " 400 ") != NULL, "non-GET request refused with 400");
  close(fd);
}

static void test_echo_lengths(void) {
  int fd = ws_connect();
  CHECK(fd >= 0, "handshake: 101 with the RFC's accept value");
  size_t sizes[] = {5, 125, 126, 200, 65535, 65536, 70000};
  int codes[] = {5, 125, 126, 126, 126, 127, 127};
  for (int k = 0; k < 7; k++) {
    uint8_t *msg = malloc(sizes[k]);
    for (size_t i = 0; i < sizes[k]; i++)
      msg[i] = (uint8_t)(i * 7 + k);
    send_frame(fd, 1, 2, msg, sizes[k], 1);
    uint8_t *p = NULL;
    size_t n = 0;
    int code = 0;
    int op = recv_frame(fd, &p, &n, &code);
    CHECK(op == 2 && n == sizes[k] && !memcmp(p, msg, n) && code == codes[k],
          "binary %zu bytes echoed, length field %d", sizes[k], code);
    free(p);
    free(msg);
  }

  send_frame(fd, 1, 1, "unmasked", 8, 0);
  uint8_t *p = NULL;
  size_t n;
  int op = recv_frame(fd, &p, &n, NULL);
  CHECK(op == 1 && n == 8 && !memcmp(p, "unmasked", 8), "unmasked client frame accepted");
  free(p);

  // A fragmented text message with a ping between its fragments.
  send_frame(fd, 0, 1, "vfo:0,0,", 8, 1);
  send_frame(fd, 1, 9, "pp", 2, 1);
  send_frame(fd, 0, 0, "7074", 4, 1);
  send_frame(fd, 1, 0, "000;", 4, 1);
  op = recv_frame(fd, &p, &n, NULL);
  CHECK(op == 10 && n == 2 && !memcmp(p, "pp", 2), "ping inside a fragmented message answered");
  free(p);
  op = recv_frame(fd, &p, &n, NULL);
  CHECK(op == 1 && !strcmp((char *)p, "vfo:0,0,7074000;"), "fragments reassembled: %s", p);
  free(p);

  send_frame(fd, 1, 8, "\x03\xe8", 2, 1);
  int before = event_count(&closes);
  CHECK(expect_close(fd, 1000), "client close 1000 answered with 1000, then the connection ends");
  CHECK(wait_for(&closes, before + 1), "on_close called");
  close(fd);
}

static void test_pipelined_first_frame(void) {
  int fd = tcp_connect();
  uint8_t buf[512];
  size_t n = strlen(request);
  memcpy(buf, request, n);
  n += make_frame(buf + n, 1, 1, "start;", 6, 1);
  send(fd, buf, n, 0);
  char head[1024];
  read_http(fd, head, sizeof(head));
  uint8_t *p = NULL;
  size_t len;
  int op = recv_frame(fd, &p, &len, NULL);
  CHECK(op == 1 && !strcmp((char *)p, "start;"), "frame sent with the handshake is read");
  free(p);
  close(fd);
}

static void test_protocol_errors(void) {
  int fd = ws_connect();
  send_frame(fd, 1, 3, "x", 1, 1);
  CHECK(expect_close(fd, 1002), "reserved opcode closes with 1002");
  close(fd);

  fd = ws_connect();
  uint8_t f[16];
  size_t n = make_frame(f, 1, 1, "x", 1, 1);
  f[0] |= 0x40; // RSV1
  send(fd, f, n, 0);
  CHECK(expect_close(fd, 1002), "RSV bit closes with 1002");
  close(fd);

  fd = ws_connect();
  send_frame(fd, 1, 0, "x", 1, 1);
  CHECK(expect_close(fd, 1002), "continuation with no message closes with 1002");
  close(fd);

  fd = ws_connect();
  size_t big = 300 * 1024;
  uint8_t *msg = calloc(1, big);
  send_frame(fd, 1, 2, msg, big, 1);
  free(msg);
  CHECK(expect_close(fd, 1009), "300 kB message closes with 1009");
  close(fd);

  // A continuation whose 64-bit length would wrap msg_len + length to 0.
  fd = ws_connect();
  send_frame(fd, 0, 1, "0123456789abcdef", 16, 1);
  uint8_t wrap[14] = {0x80, 0xFF};
  uint64_t huge = (uint64_t)0 - 16;
  for (int i = 0; i < 8; i++)
    wrap[2 + i] = (uint8_t)(huge >> (56 - 8 * i));
  send(fd, wrap, 14, 0);
  send(fd, "overflowing bytes", 17, 0);
  CHECK(expect_close(fd, 1009), "a length that would wrap the message size closes with 1009");
  close(fd);
  fd = ws_connect();
  CHECK(fd >= 0, "and the server is still serving");
  close(fd);

  fd = ws_connect();
  send_frame(fd, 1, 1, "please close", 12, 1);
  CHECK(expect_close(fd, 1000), "ws_close_client() sends 1000 and ends the connection");
  close(fd);
}

static void test_limit_and_ids(void) {
  usleep(50000); // let earlier connections finish closing
  int a = ws_connect();
  wait_for(&opens, 1);
  int id_a = event_count(&last_open_id);
  int b = ws_connect();
  int c = tcp_connect();
  send(c, request, strlen(request), 0);
  char head[512];
  read_http(c, head, sizeof(head));
  CHECK(a >= 0 && b >= 0 && strstr(head, " 503 "), "third client refused with 503 at a limit of 2");
  close(c);

  int before = event_count(&closes);
  close(a);
  wait_for(&closes, before + 1);
  int opened = event_count(&opens);
  int a2 = ws_connect();
  wait_for(&opens, opened + 1);
  int id_a2 = event_count(&last_open_id);
  CHECK(ws_client_slot(id_a2) == ws_client_slot(id_a) && id_a2 != id_a,
        "slot reused with a new id (%d -> %d)", id_a, id_a2);
  CHECK(ws_send_text(id_a, "stale") < 0, "send to the closed client's id refused");
  CHECK(ws_send_text(id_a2, "fresh") == 0, "send to the new id accepted");
  uint8_t *p = NULL;
  size_t n;
  int op = recv_frame(a2, &p, &n, NULL);
  CHECK(op == 1 && !strcmp((char *)p, "fresh"), "and only the new client gets it");
  free(p);
  close(a2);
  close(b);
}

static void test_slow_reader(void) {
  usleep(50000);
  int opened = event_count(&opens);
  int fd = ws_connect();
  int small = 4096;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  wait_for(&opens, opened + 1);
  int id = event_count(&last_open_id);
  static uint8_t chunk[16384];
  int dropped = 0, sent = 0;
  for (int i = 0; i < 1000; i++) {
    if (ws_send_binary(id, chunk, sizeof(chunk)) == 0)
      sent++;
    else
      dropped++;
  }
  CHECK(dropped > 0 && sent > 0, "client not reading: %d binary frames queued, %d dropped", sent,
        dropped);
  CHECK(ws_send_text(id, "still here") == 0, "text still accepted while binary is being dropped");
  close(fd);
}

int main(void) {
  static const struct ws_callbacks cb = {on_open, on_text, on_binary, on_close};
  for (ws_test_port = 45901; ws_test_port < 45950; ws_test_port++)
    if (ws_server_start("127.0.0.1", ws_test_port, 2, &cb) == 0)
      break;
  printf("tci_ws bench harness, loopback port %d\n", ws_test_port);

  test_accept_vector();
  test_handshake_refusals();
  test_echo_lengths();
  test_pipelined_first_frame();
  test_protocol_errors();
  test_limit_and_ids();
  test_slow_reader();

  ws_server_stop();
  printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
