// ws_test_client.h
//
// A minimal WebSocket client for the TCI bench harnesses (tci_ws_test.c,
// tci_test.c): connect and handshake, masked or unmasked frames out, frames
// in. Static functions, included by each harness; not part of maxibitx.

#ifndef WS_TEST_CLIENT_H
#define WS_TEST_CLIENT_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int ws_test_port; // the server under test

static int tcp_connect(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)ws_test_port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(fd);
    return -1;
  }
  struct timeval tv = {2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

static int read_exact(int fd, void *buf, size_t n) {
  uint8_t *p = buf;
  while (n) {
    ssize_t got = recv(fd, p, n, 0);
    if (got <= 0)
      return -1;
    p += got;
    n -= (size_t)got;
  }
  return 0;
}

// Reads the HTTP response head into buf.
static int read_http(int fd, char *buf, size_t cap) {
  size_t len = 0;
  while (len + 1 < cap) {
    if (recv(fd, buf + len, 1, 0) != 1)
      break;
    len++;
    buf[len] = '\0';
    if (len >= 4 && !memcmp(buf + len - 4, "\r\n\r\n", 4))
      return 0;
  }
  buf[len] = '\0';
  return -1;
}

static const char *request =
    "GET /tci HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

static int ws_connect(void) {
  int fd = tcp_connect();
  if (fd < 0)
    return -1;
  char head[1024];
  send(fd, request, strlen(request), 0);
  if (read_http(fd, head, sizeof(head)) < 0 || !strstr(head, " 101 ") ||
      !strstr(head, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")) {
    close(fd);
    return -1;
  }
  return fd;
}

// Builds a client frame: masked unless mask is 0.
static size_t make_frame(uint8_t *out, int fin, int op, const void *data, size_t len, int mask) {
  size_t o = 0;
  out[o++] = (uint8_t)((fin ? 0x80 : 0) | op);
  uint8_t mbit = mask ? 0x80 : 0;
  if (len < 126) {
    out[o++] = mbit | (uint8_t)len;
  } else if (len < 65536) {
    out[o++] = mbit | 126;
    out[o++] = (uint8_t)(len >> 8);
    out[o++] = (uint8_t)len;
  } else {
    out[o++] = mbit | 127;
    for (int i = 0; i < 8; i++)
      out[o++] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
  }
  uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};
  if (mask) {
    memcpy(out + o, key, 4);
    o += 4;
  }
  const uint8_t *d = data;
  for (size_t i = 0; i < len; i++)
    out[o + i] = mask ? d[i] ^ key[i & 3] : d[i];
  return o + len;
}

static int send_frame(int fd, int fin, int op, const void *data, size_t len, int mask) {
  uint8_t *buf = malloc(len + 14);
  size_t n = make_frame(buf, fin, op, data, len, mask);
  ssize_t w = send(fd, buf, n, MSG_NOSIGNAL);
  free(buf);
  return w == (ssize_t)n ? 0 : -1;
}

// Reads one server frame; returns its opcode (-1 on error) and payload.
// The header's length encoding is returned in *hdr_len_code (the 7-bit field).
static int recv_frame(int fd, uint8_t **payload, size_t *len, int *hdr_len_code) {
  uint8_t h[2];
  if (read_exact(fd, h, 2))
    return -1;
  if (h[1] & 0x80)
    return -2; // a server must not mask
  uint64_t plen = h[1] & 0x7F;
  if (hdr_len_code)
    *hdr_len_code = (int)plen;
  if (plen >= 126) {
    uint8_t e[8];
    int n = plen == 126 ? 2 : 8;
    if (read_exact(fd, e, (size_t)n))
      return -1;
    plen = 0;
    for (int i = 0; i < n; i++)
      plen = plen << 8 | e[i];
  }
  *payload = malloc(plen + 1);
  if (read_exact(fd, *payload, plen))
    return -1;
  (*payload)[plen] = 0;
  *len = plen;
  return h[0] & 0x0F;
}

#endif /* WS_TEST_CLIENT_H */
