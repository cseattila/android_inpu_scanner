#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <sys/socket.h>

static constexpr unsigned short SERVER_PORT = 6123;
static constexpr size_t MAX_PENDING_BYTES = 64 * 1024;

struct Client {
    int fd;
    std::string pending;
    size_t sent = 0;
};

static const char* event_type(unsigned short type)
{
    switch (type) {
        case EV_SYN: return "SYN";
        case EV_KEY: return "KEY";
        case EV_MSC: return "MSC";
        case EV_REL: return "REL";
        case EV_ABS: return "ABS";
        default:     return "OTHER";
    }
}

static int create_server()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::perror("setsockopt");
        close(fd);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(SERVER_PORT);

    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        std::perror("listen");
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::perror("fcntl");
        close(fd);
        return -1;
    }

    return fd;
}

static std::string format_event(const input_event& ev)
{
    char buffer[160];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%ld.%06ld  %-5s type=%02x code=%04x value=%08x",
        static_cast<long>(ev.time.tv_sec),
        static_cast<long>(ev.time.tv_usec),
        event_type(ev.type),
        ev.type,
        ev.code,
        static_cast<unsigned int>(ev.value)
    );

    std::string line(buffer);
    if (ev.type == EV_MSC && ev.code == MSC_SCAN)
        line += "  <-- IR SCAN";

    if (ev.type == EV_KEY) {
        if (ev.value == 0) line += "  UP";
        if (ev.value == 1) line += "  DOWN";
        if (ev.value == 2) line += "  REPEAT";
    }

    line += '\n';
    return line;
}

int main(int argc, char* argv[])
{
    const char* device =
        argc > 1 ? argv[1] : "/dev/input/event2";

    int fd = open(device, O_RDONLY);

    if (fd < 0) {
        std::perror("Cannot open input device");
        return 1;
    }

    int server_fd = create_server();
    if (server_fd < 0) {
        close(fd);
        return 1;
    }

    std::printf("Reading %s ...\n", device);
    std::fprintf(stderr, "TCP server listening on port %u\n", SERVER_PORT);
    std::fflush(stdout);

    std::vector<Client> clients;

    while (true) {
        std::vector<pollfd> poll_fds;
        poll_fds.reserve(clients.size() + 2);
        poll_fds.push_back({fd, POLLIN, 0});
        poll_fds.push_back({server_fd, POLLIN, 0});
        for (const Client& client : clients) {
            short events = POLLIN;
            if (client.sent < client.pending.size())
                events |= POLLOUT;
            poll_fds.push_back({client.fd, events, 0});
        }

        int ready = poll(poll_fds.data(), poll_fds.size(), -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;

            std::perror("poll");
            break;
        }

        const size_t existing_clients = clients.size();
        std::vector<bool> disconnect(existing_clients, false);

        if (poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::fprintf(stderr, "Input device is no longer available\n");
            break;
        }

        if (poll_fds[0].revents & POLLIN) {
            input_event ev;
            ssize_t n = read(fd, &ev, sizeof(ev));
            if (n < 0) {
                if (errno != EINTR) {
                    std::perror("read");
                    break;
                }
            } else if (n == 0) {
                break;
            } else if (n != sizeof(ev)) {
                std::fprintf(stderr, "Incomplete input_event\n");
            } else {
                std::string line = format_event(ev);
                std::fputs(line.c_str(), stdout);
                std::fflush(stdout);

                for (size_t i = 0; i < clients.size(); ++i) {
                    Client& client = clients[i];
                    if (client.pending.size() - client.sent + line.size() > MAX_PENDING_BYTES) {
                        if (i < existing_clients) {
                            disconnect[i] = true;
                        } else {
                            close(client.fd);
                            client.fd = -1;
                        }
                        continue;
                    }
                    if (client.sent == client.pending.size()) {
                        client.pending.clear();
                        client.sent = 0;
                    }
                    client.pending += line;
                }
            }
        }

        if (poll_fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::fprintf(stderr, "TCP listening socket failed\n");
            break;
        }

        if (poll_fds[1].revents & POLLIN) {
            while (true) {
                int client_fd = accept(server_fd, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno == EINTR)
                        continue;
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                        std::perror("accept");
                    break;
                }

                int flags = fcntl(client_fd, F_GETFL, 0);
                if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                    std::perror("fcntl client");
                    close(client_fd);
                    continue;
                }
                clients.push_back({client_fd, {}, 0});
            }
        }

        for (size_t i = 0; i < existing_clients; ++i) {
            if (disconnect[i])
                continue;

            short revents = poll_fds[i + 2].revents;
            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                disconnect[i] = true;
                continue;
            }

            if (revents & POLLIN) {
                char buffer[256];
                while (true) {
                    ssize_t n = recv(clients[i].fd, buffer, sizeof(buffer), 0);
                    if (n == 0) {
                        disconnect[i] = true;
                        break;
                    }
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                            disconnect[i] = true;
                        break;
                    }
                }
            }

            if (!disconnect[i] && (revents & POLLOUT)) {
                const char* data = clients[i].pending.data() + clients[i].sent;
                size_t remaining = clients[i].pending.size() - clients[i].sent;
                ssize_t n = send(clients[i].fd, data, remaining, MSG_NOSIGNAL);
                if (n > 0) {
                    clients[i].sent += static_cast<size_t>(n);
                    if (clients[i].sent == clients[i].pending.size()) {
                        clients[i].pending.clear();
                        clients[i].sent = 0;
                    }
                } else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    disconnect[i] = true;
                }
            }
        }

        for (size_t i = existing_clients; i > 0; --i) {
            if (disconnect[i - 1]) {
                close(clients[i - 1].fd);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i - 1));
            }
        }

        for (auto it = clients.begin(); it != clients.end();) {
            if (it->fd < 0) {
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const Client& client : clients)
        close(client.fd);
    close(server_fd);
    close(fd);
    return 0;
}