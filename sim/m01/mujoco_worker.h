#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <sys/types.h>

namespace waste::m01sim {

// One headless worker per run. A local socket carries the private, bounded
// command/step protocol without starting a shell.
class MujocoWorker {
public:
    explicit MujocoWorker(const std::string& mode);
    ~MujocoWorker();
    MujocoWorker(const MujocoWorker&) = delete;
    MujocoWorker& operator=(const MujocoWorker&) = delete;

    void submit(std::string_view command);
    std::vector<std::string> advance(long tick_ms);

private:
    void send(std::string_view message);
    std::string line();
    void close();
    int socket_{-1};
    pid_t pid_{-1};
    std::string buffer_;
};

}  // namespace waste::m01sim
