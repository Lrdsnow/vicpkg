#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#define SOCKET_PATH      "/tmp/akalsasink_audio.sock"
#define CHUNK_SIZE       1024
#define SAMPLE_RATE      32000
#define CHANNELS         1
#define BYTES_PER_SAMPLE 2
#define FRAME_BYTES      (CHUNK_SIZE * CHANNELS * BYTES_PER_SAMPLE)
#define PREFILL_CHUNKS   3

static long now_ns() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000L + t.tv_nsec;
}

static void sleep_ns(long ns) {
    if (ns <= 0) return;
    struct timespec ts;
    ts.tv_sec  = ns / 1000000000L;
    ts.tv_nsec = ns % 1000000000L;
    nanosleep(&ts, NULL);
}

static int send_all(int sock, unsigned char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            fprintf(stderr, "send failed at offset %d: errno=%d (%s)\n",
                    sent, errno, strerror(errno));
            fflush(stderr);
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int has_ffmpeg() {
    int status = system("ffmpeg -version >/dev/null 2>&1");
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int is_raw_file(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 0;
    return strcmp(ext, ".raw") == 0 || strcmp(ext, ".pcm") == 0;
}

static FILE* open_audio_file(const char *filename) {
    if (is_raw_file(filename)) {
        return fopen(filename, "rb");
    }

    if (!has_ffmpeg()) {
        fprintf(stderr, "Error: File is not .raw/.pcm and ffmpeg is not available\n");
        return NULL;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ffmpeg -i '%s' -f s16le -ar %d -ac %d -",
             filename, SAMPLE_RATE, CHANNELS);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return NULL;
    }

    return fp;
}

static void close_audio_file(FILE *f, const char *filename) {
    if (is_raw_file(filename)) {
        fclose(f);
    } else {
        pclose(f);
    }
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <audio_file>\n", argv[0]);
        return 1;
    }

    FILE *f = open_audio_file(argv[1]);
    if (!f) {
        fprintf(stderr, "Failed to open audio file: %s\n", argv[1]);
        return 1;
    }

    fprintf(stdout, "Opened audio file: %s\n", argv[1]);
    fflush(stdout);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        close_audio_file(f, argv[1]);
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close_audio_file(f, argv[1]);
        return 1;
    }
    fprintf(stdout, "Connected to socket\n");
    fflush(stdout);

    long chunk_duration_ns = (long)((double)CHUNK_SIZE / SAMPLE_RATE * 1000000000.0);

    unsigned char buf[FRAME_BYTES];
    int prefilled = 0;
    long start_ns = 0;
    long chunk_index = 0;
    long chunks_sent = 0;

    while (1) {
        size_t n = fread(buf, 1, FRAME_BYTES, f);
        if (n == 0) {
            fprintf(stdout, "EOF after %ld chunks (%.1f seconds)\n",
                    chunks_sent, (double)chunks_sent * CHUNK_SIZE / SAMPLE_RATE);
            fflush(stdout);
            break;
        }
        if (n < FRAME_BYTES)
            memset(buf + n, 0, FRAME_BYTES - n);

        if (prefilled < PREFILL_CHUNKS) {
            fprintf(stdout, "Sending prefill chunk %d\n", prefilled);
            fflush(stdout);
            if (send_all(sock, buf, FRAME_BYTES) < 0) {
                fprintf(stderr, "send failed during prefill chunk %d\n", prefilled);
                fflush(stderr);
                goto done;
            }
            prefilled++;
            chunks_sent++;
            if (prefilled == PREFILL_CHUNKS) {
                start_ns = now_ns();
                chunk_index = 0;
                fprintf(stdout, "Prefill done, starting paced playback\n");
                fflush(stdout);
            }
            continue;
        }

        chunk_index++;
        long target_ns = start_ns + chunk_index * chunk_duration_ns;
        long wait = target_ns - now_ns();
        sleep_ns(wait);

        if (send_all(sock, buf, FRAME_BYTES) < 0) {
            fprintf(stderr, "send failed at chunk %ld (%.1f seconds in)\n",
                    chunks_sent, (double)chunks_sent * CHUNK_SIZE / SAMPLE_RATE);
            fflush(stderr);
            goto done;
        }
        chunks_sent++;

        if (chunks_sent % 10 == 0) {
            fprintf(stdout, "Progress: %.1f seconds sent\n",
                    (double)chunks_sent * CHUNK_SIZE / SAMPLE_RATE);
            fflush(stdout);
        }
    }

    {
        const int SILENCE_CHUNKS = 8;
        memset(buf, 0, FRAME_BYTES);
        fprintf(stdout, "Sending %d silence chunks\n", SILENCE_CHUNKS);
        fflush(stdout);
        for (int i = 0; i < SILENCE_CHUNKS; i++) {
            chunk_index++;
            long target_ns = start_ns + chunk_index * chunk_duration_ns;
            long wait = target_ns - now_ns();
            sleep_ns(wait);
            if (send_all(sock, buf, FRAME_BYTES) < 0) break;
        }
    }

done:
    close_audio_file(f, argv[1]);
    close(sock);
    fprintf(stdout, "Done. Total chunks sent: %ld (%.1f seconds)\n",
            chunks_sent, (double)chunks_sent * CHUNK_SIZE / SAMPLE_RATE);
    fflush(stdout);
    return 0;
}