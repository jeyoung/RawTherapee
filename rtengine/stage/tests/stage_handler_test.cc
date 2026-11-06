#include "stage_handler.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

/*
 * A guard failure is a fatal error, so it can only be observed from another
 * process. Run the body in a child with its stderr captured, and report the
 * termination status and the fatal message the child wrote.
 */
struct ChildResult {
    bool signalled = false;
    int signal = 0;
    std::string stderr_text;
};

template <typename Body> ChildResult runInChild(Body body)
{
    ChildResult result;

    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        std::perror("stage-handler-test: pipe");
        return result;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        std::perror("stage-handler-test: fork");
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return result;
    }

    if (pid == 0) {
        ::close(pipe_fds[0]);
        if (::dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            std::_Exit(2);
        }
        ::close(pipe_fds[1]);

        // Do not leave core files behind for an expected abort.
        const rlimit no_core = {0, 0};
        (void)::setrlimit(RLIMIT_CORE, &no_core);

        body();
        std::_Exit(0);
    }

    ::close(pipe_fds[1]);
    char buffer[256];
    for (;;) {
        const ssize_t count = ::read(pipe_fds[0], buffer, sizeof(buffer));
        if (count > 0) {
            result.stderr_text.append(buffer, static_cast<size_t>(count));
        } else if (count == 0) {
            break;
        } else {
            std::perror("stage-handler-test: read");
            break;
        }
    }
    ::close(pipe_fds[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        std::perror("stage-handler-test: waitpid");
        return result;
    }
    result.signalled = WIFSIGNALED(status);
    if (result.signalled) {
        result.signal = WTERMSIG(status);
    }
    return result;
}

bool diedOfSigabrt(const ChildResult& result)
{
    return result.signalled && result.signal == SIGABRT;
}

bool reportedFatal(const ChildResult& result)
{
    return result.stderr_text.find("stage-handler: fatal:") !=
           std::string::npos;
}

} // namespace

int main()
{
    using FunctionRegistry =
        rtengine::stage::StageHandlerRegistry<std::function<int()>>;
    using IntegerRegistry = rtengine::stage::StageHandlerRegistry<int>;

    FunctionRegistry functions;
    functions.add("first", [] { return 1; });
    functions.add("second", [] { return 2; });
    functions.add("third", [] { return 3; });

    check(functions.ids() ==
              std::vector<std::string>({"first", "second", "third"}),
          "ids preserve insertion order");
    check(functions.entries().size() == 3, "every entry is stored once");
    check(functions.contains("first"), "contains finds the first id");
    check(functions.contains("second"), "contains finds a middle id");
    check(functions.contains("third"), "contains finds the last id");
    check(!functions.contains("missing"), "contains rejects an unknown id");
    check(!functions.contains(""), "contains rejects an empty id");

    const std::function<int()> second = functions.find("second");
    check(static_cast<bool>(second), "find returns a stored handler");
    check(static_cast<bool>(second) && second() == 2,
          "find returns the handler for its own id");
    check(!functions.find("missing"), "missing find returns an empty handler");

    IntegerRegistry integers;
    integers.add("alpha", 10);
    integers.add("beta", 20);

    check(integers.ids() == std::vector<std::string>({"alpha", "beta"}),
          "integer ids preserve insertion order");
    check(integers.contains("beta"), "integer contains finds a known id");
    check(!integers.contains("gamma"), "integer contains rejects an unknown id");
    check(integers.find("alpha") == 10,
          "integer find returns the handler for its own id");
    check(integers.find("missing") == 0,
          "missing integer find returns the default handler");

    const ChildResult duplicateFunction = runInChild([] {
        FunctionRegistry registry;
        registry.add("duplicate", [] { return 1; });
        registry.add("duplicate", [] { return 2; });
    });
    check(diedOfSigabrt(duplicateFunction),
          "duplicate std::function id terminates with SIGABRT");
    check(reportedFatal(duplicateFunction),
          "duplicate std::function id reports the fatal guard");

    const ChildResult duplicateInteger = runInChild([] {
        IntegerRegistry registry;
        registry.add("duplicate", 1);
        registry.add("duplicate", 2);
    });
    check(diedOfSigabrt(duplicateInteger),
          "duplicate integer id terminates with SIGABRT");
    check(reportedFatal(duplicateInteger),
          "duplicate integer id reports the fatal guard");

    const ChildResult nullId = runInChild([] {
        FunctionRegistry registry;
        registry.add(nullptr, [] { return 1; });
    });
    check(diedOfSigabrt(nullId), "null id terminates with SIGABRT");
    check(reportedFatal(nullId), "null id reports the fatal guard");

    const ChildResult emptyId = runInChild([] {
        FunctionRegistry registry;
        registry.add("", [] { return 1; });
    });
    check(diedOfSigabrt(emptyId), "empty id terminates with SIGABRT");
    check(reportedFatal(emptyId), "empty id reports the fatal guard");

    if (failures == 0) {
        std::puts("stage-handler-test: PASS");
    }
    return failures == 0 ? 0 : 1;
}
