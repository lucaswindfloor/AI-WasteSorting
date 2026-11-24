#include "mujoco_worker.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace waste::m01sim {

MujocoWorker::MujocoWorker(const std::string& mode) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
        throw std::runtime_error("could not open a MuJoCo worker socket");
    }
    posix_spawn_file_actions_t actions;
    auto code = posix_spawn_file_actions_init(&actions);
    if (code == 0) {
        code = posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
        if (code == 0) code = posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO);
        if (code == 0) {
            auto* configured = std::getenv("M01_SIM_MUJOCO_PYTHON");
            std::string python = configured && *configured ? configured : "python3";
            std::string module = "sim.mujoco.controller", flag = "-m", scenario = mode;
            char* args[] = {python.data(), flag.data(), module.data(), scenario.data(), nullptr};
            code = ::posix_spawnp(&pid_, python.c_str(), &actions, nullptr, args, environ);
        }
        posix_spawn_file_actions_destroy(&actions);
    }
    ::close(pair[1]);
    socket_ = pair[0];
    if (code != 0) {
        close();
        throw std::runtime_error("could not launch the MuJoCo Python worker");
    }
    try {
        if (line() != "ready") throw std::runtime_error("MuJoCo worker did not start");
    } catch (...) {
        close();
        throw;
    }
}

MujocoWorker::~MujocoWorker() { close(); }

void MujocoWorker::close() {
    if (socket_ != -1) ::close(socket_);
    socket_ = -1;
    if (pid_ > 0) {
        if (::waitpid(pid_, nullptr, WNOHANG) == 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
        pid_ = -1;
    }
}

void MujocoWorker::send(std::string_view message) {
    while (!message.empty()) {
        const auto written = ::send(socket_, message.data(), message.size(), MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::runtime_error("MuJoCo worker command channel closed");
        message.remove_prefix(static_cast<std::size_t>(written));
    }
}

std::string MujocoWorker::line() {
    while (true) {
        const auto end = buffer_.find('\n');
        if (end != std::string::npos) {
            auto result = buffer_.substr(0, end);
            buffer_.erase(0, end + 1);
            return result;
        }
        pollfd wait{socket_, POLLIN, 0};
        const auto ready = ::poll(&wait, 1, 10000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) throw std::runtime_error("MuJoCo worker did not respond");
        char chunk[4096];
        const auto n = ::recv(socket_, chunk, sizeof(chunk), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("MuJoCo worker closed; check the terminal and Python environment");
        buffer_.append(chunk, static_cast<std::size_t>(n));
        if (buffer_.size() > 16384) throw std::runtime_error("MuJoCo worker line is too long");
    }
}

void MujocoWorker::submit(std::string_view command) {
    send(command);
    if (line() != "accepted") throw std::runtime_error("MuJoCo worker did not accept the command");
}

std::vector<std::string> MujocoWorker::advance(long tick_ms) {
    send("step " + std::to_string(tick_ms) + "\n");
    std::vector<std::string> result;
    while (result.size() < 32) {
        auto record = line();
        if (record == ".") return result;
        result.push_back(std::move(record));
    }
    throw std::runtime_error("too many MuJoCo records in a step");
}

}  // namespace waste::m01sim
