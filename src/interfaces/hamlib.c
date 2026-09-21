// hamlib.c - see hamlib.h for scope.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // strcasecmp() - name_to_mode() below
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "hamlib.h"
#include "cw.h"
#include "radio.h"    // freq_hdr, in_tx, tuning, PTT, RIT, mode
#include "rx_audio.h"
#include "sound.h"    // sound_set_mic_tx_gain()/sound_get_mic_tx_gain() - l/L MICGAIN below

static int listen_fd = -1;
static volatile int running = 0;
static pthread_t accept_thread;

// Mode lives in radio.c (radio_get_mode()/radio_set_mode()); these
// translate to and from Hamlib's RIG_MODE names. DIGITAL goes out as
// PKTUSB, the standard Hamlib name for USB with a data modem - not yet
// checked against a stock Hamlib client over this server.
static const struct { enum radio_mode mode; const char *name; } mode_names[] = {
    { RADIO_MODE_CW,      "CW"     },
    { RADIO_MODE_USB,     "USB"    },
    { RADIO_MODE_LSB,     "LSB"    },
    { RADIO_MODE_DIGITAL, "PKTUSB" },
};
#define MODE_NAMES_COUNT (sizeof(mode_names) / sizeof(mode_names[0]))

static const char *mode_to_name(enum radio_mode m)
{
    for (size_t i = 0; i < MODE_NAMES_COUNT; i++)
        if (mode_names[i].mode == m)
            return mode_names[i].name;
    return "CW"; // unreachable given the enum's own values; a safe fallback if that ever changes
}

// Returns 1 and sets *out for a recognized name, else 0 (the caller then
// replies RPRT -1 rather than misreport the mode). Also accepts "DIGITAL",
// which tools/rigctl_panel.py sends; replies still say PKTUSB, and the
// panel maps that back to DIGITAL.
static int name_to_mode(const char *name, enum radio_mode *out)
{
    if (strcasecmp(name, "DIGITAL") == 0) {
        *out = RADIO_MODE_DIGITAL;
        return 1;
    }
    for (size_t i = 0; i < MODE_NAMES_COUNT; i++) {
        if (strcasecmp(name, mode_names[i].name) == 0) {
            *out = mode_names[i].mode;
            return 1;
        }
    }
    return 0;
}

// Accepted by M and echoed back by m, but not applied - the RX passband
// is fixed (rx_audio.c).
static int current_passband = 2400;

#define LINE_MAX_LEN 256

static void send_line(int fd, const char *s)
{
    // Best-effort - a client that vanished mid-command isn't fatal,
    // MSG_NOSIGNAL keeps a dead peer from raising SIGPIPE.
    send(fd, s, strlen(s), MSG_NOSIGNAL);
}

static void send_rprt(int fd, int code)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "RPRT %d\n", code);
    send_line(fd, buf);
}

// Handles one already-newline-stripped command line for one connection.
// Returns 0 to keep the connection open, -1 to close it (q/Q).
static int handle_line(int fd, char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) {
        line[--len] = '\0';
    }
    if (len == 0) return 0;

    // Extended-response prefix ('+') - we don't implement the labeled
    // echo format some clients can request, just accept and ignore the
    // prefix so a client that defaults to sending it still gets a
    // normal reply instead of an unknown-command error.
    char *cmd = line;
    if (cmd[0] == '+') cmd++;
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "Q") == 0 ||
        strcmp(cmd, "quit") == 0) {
        printf("rigctl: %s -> closing connection\n", cmd);
        return -1;
    }

    if (cmd[0] == 'f' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_freq
        char buf[32];
        snprintf(buf, sizeof(buf), "%d\n", freq_hdr);
        send_line(fd, buf);
        printf("rigctl: f -> %d Hz\n", freq_hdr);
        return 0;
    }

    if (cmd[0] == 'F' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // set_freq <hz>
        long f = strtol(cmd + 1, NULL, 10);
        if (f <= 0) {
            send_rprt(fd, -1);
            printf("rigctl: F%s -> invalid frequency, ignored\n", cmd + 1);
            return 0;
        }
        radio_tune_to((uint32_t)f);
        send_rprt(fd, 0);
        printf("rigctl: F %ld -> tuned to %ld Hz\n", f, f);
        return 0;
    }

    if (cmd[0] == 't' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_ptt
        char buf[8];
        snprintf(buf, sizeof(buf), "%d\n", in_tx ? 1 : 0);
        send_line(fd, buf);
        printf("rigctl: t -> %s\n", in_tx ? "TX" : "RX");
        return 0;
    }

    if (cmd[0] == 'T' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // set_ptt <0|1|2|3> - one TX state here, no mic/data distinction,
        // so any nonzero value means TX.
        long v = strtol(cmd + 1, NULL, 10);
        int tx_on = (v != 0);
        if (cw_tx_active()) {
            send_rprt(fd, 0);
            printf("rigctl: T %ld -> ignored, local CW key holds TX\n", v);
            return 0;
       }
        radio_set_tx(tx_on);
        send_rprt(fd, 0);
        printf("rigctl: T %ld -> %s\n", v, tx_on ? "TX on" : "TX off");
        return 0;
    }

    if (cmd[0] == 'j' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_rit - a signed Hz offset, same convention as get_freq. 0
        // means no RIT applied, matching how real Hamlib rigs treat RIT
        // (there's no separate on/off bit in the core protocol - see J
        // below).
        char buf[16];
        snprintf(buf, sizeof(buf), "%d\n", radio_get_rit());
        send_line(fd, buf);
        printf("rigctl: j -> %d Hz\n", radio_get_rit());
        return 0;
    }

    if (cmd[0] == 'J' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // set_rit <hz> - RX-only offset, +/-RIT_MAX_HZ. "J 0" turns RIT off
        // (rigctld has no separate on/off). Cleared by the next F.
        long hz = strtol(cmd + 1, NULL, 10);
        if (hz < -RIT_MAX_HZ || hz > RIT_MAX_HZ) {
            send_rprt(fd, -1);
            printf("rigctl: J %ld -> out of range (+/-%d Hz), ignored\n", hz, RIT_MAX_HZ);
            return 0;
        }
        radio_set_rit((int)hz);
        send_rprt(fd, 0);
        printf("rigctl: J %ld -> RIT %+ld Hz\n", hz, hz);
        return 0;
    }

    if (cmd[0] == 'm' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_mode - two lines: mode, then passband (see current_passband).
        char buf[64];
        const char *name = mode_to_name(radio_get_mode());
        snprintf(buf, sizeof(buf), "%s\n%d\n", name, current_passband);
        send_line(fd, buf);
        printf("rigctl: m -> %s %d\n", name, current_passband);
        return 0;
    }

    if (cmd[0] == 'M' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // set_mode <mode> <passband> - an unknown mode name gets RPRT -1.
        // The passband is stored and echoed by m, not applied.
        char mode[32] = "";
        int passband = current_passband;
        sscanf(cmd + 1, "%31s %d", mode, &passband);
        enum radio_mode m;
        if (mode[0] && !name_to_mode(mode, &m)) {
            send_rprt(fd, -1);
            printf("rigctl: M %s -> unrecognized mode, ignored\n", mode);
            return 0;
        }
        if (mode[0])
            radio_set_mode(m);
        current_passband = passband;
        send_rprt(fd, 0);
        printf("rigctl: M %s %d -> ok\n",
               mode_to_name(radio_get_mode()), current_passband);
        return 0;
    }

    if (cmd[0] == 'l' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_level <name>. AF and STRENGTH are real Hamlib levels (see
        // dump_state); MICGAIN is this server's extension. Anything else
        // (RF, SQL, preamp, ...) has no equivalent here.
        char level_name[32] = "";
        sscanf(cmd + 1, "%31s", level_name);
        if (strcmp(level_name, "AF") == 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6f\n", rx_audio_get_volume() / 100.0);
            send_line(fd, buf);
            printf("rigctl: l AF -> %d%%\n", rx_audio_get_volume());
        } else if (strcmp(level_name, "STRENGTH") == 0) {
            int db = rx_audio_get_strength_db();
            char buf[16];
            snprintf(buf, sizeof(buf), "%d\n", db);
            send_line(fd, buf);
            printf("rigctl: l STRENGTH -> %d (dB relative to S9)\n", db);
        } else if (strcmp(level_name, "MICGAIN") == 0) {
            // Extension, not a Hamlib RIG_LEVEL: the raw mic_tx_gain multiplier
            // (sound.h), not 0.0-1.0, since it has no natural 100%. Under l/L
            // rather than u/U because it's a continuous value.
            char buf[32];
            snprintf(buf, sizeof(buf), "%.6f\n", sound_get_mic_tx_gain());
            send_line(fd, buf);
            printf("rigctl: l MICGAIN -> %.6f\n", sound_get_mic_tx_gain());
        } else {
            send_rprt(fd, -1);
            printf("rigctl: l %s -> unsupported level\n", level_name);
        }
        return 0;
    }

    if (cmd[0] == 'L' && cmd[1] == ' ') {
        // set_level <name> <value>. STRENGTH is read-only, like a real
        // S-meter, so "L STRENGTH" falls through to the error reply.
        char level_name[32] = "";
        double val = 0.0;
        if (sscanf(cmd + 1, "%31s %lf", level_name, &val) == 2 &&
            strcmp(level_name, "AF") == 0) {
            int percent = (int)(val * 100.0 + 0.5);
            rx_audio_set_volume(percent);
            send_rprt(fd, 0);
            printf("rigctl: L AF %.6f -> volume %d%%\n", val, percent);
        } else if (sscanf(cmd + 1, "%31s %lf", level_name, &val) == 2 &&
                   strcmp(level_name, "MICGAIN") == 0) {
            sound_set_mic_tx_gain(val);  // clamps to [0, SOUND_MIC_TX_GAIN_MAX] itself
            send_rprt(fd, 0);
            printf("rigctl: L MICGAIN %.6f -> mic_tx_gain %.6f\n", val, sound_get_mic_tx_gain());
        } else {
            send_rprt(fd, -1);
            printf("rigctl: L %s -> unsupported level or bad args\n", cmd + 1);
        }
        return 0;
    }

    if (cmd[0] == 'u' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_func <name>: NARROW (rx_audio.c's narrow stage-3 filter, on/off)
        // and FFTFILT (which stage-3 implementation it uses). Not Hamlib
        // RIG_FUNC names - extensions for tools/rigctl_panel.py.
        char func_name[32] = "";
        sscanf(cmd + 1, "%31s", func_name);
        if (strcmp(func_name, "NARROW") == 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d\n", rx_audio_get_narrow_filter());
            send_line(fd, buf);
            printf("rigctl: u NARROW -> %s\n",
                   rx_audio_get_narrow_filter() ? "on" : "off");
        } else if (strcmp(func_name, "FFTFILT") == 0) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d\n", rx_audio_get_narrow_filter_impl());
            send_line(fd, buf);
            printf("rigctl: u FFTFILT -> %s\n",
                   rx_audio_get_narrow_filter_impl() ? "fft" : "elliptic");
        } else {
            send_rprt(fd, -1);
            printf("rigctl: u %s -> unsupported function\n", func_name);
        }
        return 0;
    }

    if (cmd[0] == 'U' && cmd[1] == ' ') {
        // set_func <name> <0|1>
        char func_name[32] = "";
        int val = 0;
        if (sscanf(cmd + 1, "%31s %d", func_name, &val) != 2) {
            send_rprt(fd, -1);
            printf("rigctl: U %s -> bad args\n", cmd + 1);
        } else if (strcmp(func_name, "NARROW") == 0) {
            rx_audio_set_narrow_filter(val != 0);
            send_rprt(fd, 0);
            printf("rigctl: U NARROW %d -> narrow filter %s\n", val,
                   val ? "on" : "off");
        } else if (strcmp(func_name, "FFTFILT") == 0) {
            rx_audio_set_narrow_filter_impl(val != 0);
            send_rprt(fd, 0);
            printf("rigctl: U FFTFILT %d -> stage-3 implementation %s\n", val,
                   val ? "fft" : "elliptic");
        } else {
            send_rprt(fd, -1);
            printf("rigctl: U %s -> unsupported function\n", cmd + 1);
        }
        return 0;
    }

    if (cmd[0] == 'v' && (cmd[1] == '\0' || cmd[1] == ' ')) {
        // get_vfo - maxibitx has one VFO; always report it
        send_line(fd, "VFOA\n");
        printf("rigctl: v -> VFOA\n");
        return 0;
    }
    if (cmd[0] == 'V' && cmd[1] == ' ') {
        // set_vfo <vfo> - one VFO, nothing to switch to: accept any name.
        send_rprt(fd, 0);
        printf("rigctl: V %s -> ok (single VFO)\n", cmd + 2);
        return 0;
    }

    if (strcmp(cmd, "chk_vfo") == 0 || strcmp(cmd, "\\chk_vfo") == 0) {
        // Single-VFO radio - report "not in VFO mode" so callers send
        // plain f/F/t/T without needing a VFO argument.
        send_line(fd, "0\n");
        printf("rigctl: chk_vfo -> 0\n");
        return 0;
    }

    if (strcmp(cmd, "dump_state") == 0 || strcmp(cmd, "\\dump_state") == 0) {
        // Minimal, spec-shaped dump_state (format checked against Hamlib's
        // rigctl_parse.c). Advertises only what exists: no XIT/IF shift, no
        // preamp/attenuator, no filter list; max_rit is real (RIT_MAX_HZ).
        // has_get_level = RIG_LEVEL_AF (0x8) | RIG_LEVEL_STRENGTH (1<<30) =
        // 0x40000008; has_set_level is AF only (an S-meter can't be set).
        // has_get_func/set_func stay 0: NARROW/FFTFILT aren't RIG_FUNC bits.
        //
        // The TX range list is empty - left from before maxibitx could
        // transmit, even though t/T keys PTT. tools/rigctl_panel.py ignores it;
        // untested with a stock Hamlib client.
        send_line(fd, "0\n");                        // protocol version
        send_line(fd, "1\n");                        // rig model (1 = RIG_MODEL_DUMMY)
        send_line(fd, "2\n");                         // ITU region (best-effort default)
        send_line(fd, "0 30000000 0x1ff -1 -1 0x1 0x0\n"); // RX range: 0-30MHz, all modes, RX-only, VFO A
        send_line(fd, "0 0 0 0 0 0 0\n");             // RX range list terminator
        send_line(fd, "0 0 0 0 0 0 0\n");             // empty TX range list
        send_line(fd, "0x1ff 1\n");                   // one tuning step: 1 Hz, all modes
        send_line(fd, "0 0\n");                       // tuning step list terminator
        send_line(fd, "0 0\n");                       // empty filter list
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d\n", RIT_MAX_HZ);
            send_line(fd, buf);                        // max_rit
        }
        send_line(fd, "0\n");                         // max_xit
        send_line(fd, "0\n");                         // max_ifshift
        send_line(fd, "0\n");                         // announces
        send_line(fd, "\n");                          // preamp list (empty)
        send_line(fd, "\n");                          // attenuator list (empty)
        send_line(fd, "0x0\n");                       // has_get_func
        send_line(fd, "0x0\n");                       // has_set_func
        send_line(fd, "0x40000008\n");                // has_get_level (RIG_LEVEL_AF | RIG_LEVEL_STRENGTH)
        send_line(fd, "0x8\n");                       // has_set_level (RIG_LEVEL_AF only)
        send_line(fd, "0x0\n");                       // has_get_parm
        send_line(fd, "0x0\n");                       // has_set_parm
        printf("rigctl: dump_state -> sent\n");
        return 0;
    }

    // Unknown command - reply cleanly instead of hanging the client.
    send_rprt(fd, -1);
    printf("rigctl: %s -> unknown command\n", cmd);
    return 0;
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;

    char buf[LINE_MAX_LEN];
    size_t buf_len = 0;

    while (running) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) break;   // disconnected or error

        if (c == '\n') {
            buf[buf_len] = '\0';
            if (handle_line(fd, buf) < 0) break;
            buf_len = 0;
        } else if (buf_len + 1 < sizeof(buf)) {
            buf[buf_len++] = c;
        }
        // else: line too long - silently drop extra bytes until '\n'
    }

    close(fd);
    printf("hamlib: client disconnected\n");
    return NULL;
}

static void *accept_thread_fn(void *arg)
{
    (void)arg;
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int fd = accept(listen_fd, (struct sockaddr *)&client_addr, &len);
        if (fd < 0) {
            if (!running) break;
            continue;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        printf("hamlib: client connected from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, (void *)(intptr_t)fd) == 0) {
            pthread_detach(tid);
        } else {
            close(fd);
        }
    }
    return NULL;
}

int hamlib_init(int port)
{
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf("hamlib: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    int optval = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("hamlib: bind() to port %d failed: %s\n", port, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }

    if (listen(listen_fd, 4) < 0) {
        printf("hamlib: listen() failed: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }

    running = 1;
    if (pthread_create(&accept_thread, NULL, accept_thread_fn, NULL) != 0) {
        printf("hamlib: failed to start accept thread\n");
        running = 0;
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }
    pthread_detach(accept_thread);
    printf("init: Hamlib/rigctld listening on TCP %d\n", port);
    return 0;
}

void hamlib_stop(void)
{
    running = 0;
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}
