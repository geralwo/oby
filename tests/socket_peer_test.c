/* TCP sockets across processes: this test listens with raw sockets, forks a
   client script that connects to it, serves it, and checks what the client
   printed. (The harness runs tests serially, so the test drives both sides.) */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define DIR    "build/socket_peer"
#define SCRIPT DIR "/client.oby"
#define OUT    DIR "/out.txt"

static int fails = 0;

static void check(const char *what, int ok) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static int write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    fputs(text, file);
    return fclose(file) == 0;
}

/* Read a whole file; NULL on failure. Caller frees. */
static char *read_all(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, file);
    fclose(file);
    buf[got] = '\0';
    return buf;
}

int main(void) {
    check("scratch directory", system("mkdir -p " DIR) == 0);
    check("client written",
          write_file(SCRIPT, "Socket s = Socket::connect(\"127.0.0.1\", args()[2].to_int())\n"
                             "s.write(\"hello\\n\")\n"
                             "print(\"reply: ${s.read_line()}\")\n"
                             "s.close()\n"));

    /* A listening socket on an ephemeral port, so the test cannot collide with
       anything else on the machine. */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    check("server socket", srv >= 0);
    int on = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    check("bind", bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0);
    check("listen", listen(srv, 4) == 0);
    socklen_t alen = sizeof addr;
    check("getsockname", getsockname(srv, (struct sockaddr *)&addr, &alen) == 0);
    char port[16];
    snprintf(port, sizeof port, "%d", ntohs(addr.sin_port));

    /* The client runs in a child, so the parent can serve it. */
    pid_t pid = fork();
    if (pid == 0) {
        freopen(OUT, "w", stdout);
        execl("build/oby", "oby", SCRIPT, port, (char *)NULL);
        _exit(127);
    }
    check("client forked", pid > 0);

    int cli = accept(srv, NULL, NULL);
    check("accepted the client", cli >= 0);
    if (cli >= 0) {
        char buf[64];
        ssize_t n = read(cli, buf, sizeof buf - 1);
        if (n < 0) n = 0;
        buf[n] = '\0';
        check("client sent its line", strcmp(buf, "hello\n") == 0);
        const char *reply = "hi from the test\n";
        check("reply written",
              write(cli, reply, strlen(reply)) == (ssize_t)strlen(reply));
        close(cli);
    }
    close(srv);

    int status = 0;
    waitpid(pid, &status, 0);
    check("client exited 0", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    char *out = read_all(OUT);
    check("client saw the reply", out && strcmp(out, "reply: hi from the test\n") == 0);
    if (out && strcmp(out, "reply: hi from the test\n") != 0) printf("  got: %s", out);
    free(out);

    system("rm -rf " DIR);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
