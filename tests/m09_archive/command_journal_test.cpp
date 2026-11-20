#include "fixtures.h"
#include "m09_archive/storage/command_journal.h"
#include "m09_archive/storage/record_codec.h"

#include <sys/wait.h>
#include <unistd.h>

namespace waste::m09::test {

void command_journal() {
    TemporaryArchive temp;
    const auto path = temp.path / "critical";
    const Checkpoint one{1, "writer-1/1", {std::byte{1}, std::byte{9}}, {{"intent-042", "original"}}};
    const auto child = ::fork();
    check(child >= 0, "fork failed");
    if (child == 0) {
        const auto result = CommandJournal(path).commit(0, one);
        ::_exit(result == Status::Ok ? 0 : 1);
    }
    int exit_code{};
    check(::waitpid(child, &exit_code, 0) == child && WIFEXITED(exit_code) && WEXITSTATUS(exit_code) == 0,
          "child could not persist checkpoint before exit");
    const CommandJournal restarted(path);
    check(restarted.load().value == one, "new process could not recover exact bytes");
    check(restarted.commit(0, one) == Status::Duplicate, "identical write after losing reply failed");
    auto conflict = one;
    conflict.state.front() = std::byte{8};
    check(restarted.commit(0, conflict) == Status::Conflict, "same revision/content conflict lost");
    check(restarted.commit(0, {2, "writer-1/2", one.state}) == Status::Invalid, "invalid revision advancement");
    check(restarted.commit(1, {2, "writer-1/2", one.state, {{"intent-042", "changed"}}}) == Status::Conflict,
          "same request could change its bytes in a later checkpoint");
    check(restarted.commit(1, {2, "writer-1/2", one.state}) == Status::Ok &&
          restarted.load(1).value == one, "exact historical checkpoint lost");
    std::filesystem::resize_file(path / "2.checkpoint", 5);
    check(restarted.load().status == Status::Corrupt &&
          restarted.commit(2, {3, "writer-1/3", one.state}) == Status::Corrupt,
          "corrupt newest command allowed new transmission state");
    std::filesystem::remove(path / "head");
    check(restarted.load().status == Status::Corrupt && restarted.commit(0, one) == Status::Corrupt,
          "lost head permitted old request identity to be reused");
}

}  // namespace waste::m09::test
