// libc functions a native PS5 app imports but cannot use.
//
// Inside an app, the runtime linker leaves imports from modules it did not
// load as NULL (libkernel_sys, libScePosixForWebKit), and some functions that
// do resolve jump to unresolved internals of their own module (getcwd in
// libSceLibcInternal). libc++'s std::filesystem reaches several of them:
// current_path() calls pathconf and getcwd, read_symlink/canonical readlink,
// create_symlink/create_hard_link symlink/link. The first PS5 run of WoWPS
// jumped to address 0 from __current_path. Definitions here win over the
// imports at static link time; --exclude-libs keeps them out of the exports.
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <unistd.h>

#include "console_paths.hpp"

extern "C" {

long pathconf(const char* path, int name) {
    (void)path;
    switch (name) {
    case _PC_PATH_MAX: return 1024;
    case _PC_NAME_MAX: return 255;
    case _PC_LINK_MAX: return 32767;
    default: errno = EINVAL; return -1;
    }
}

// The process never changes directory: the app root is its working directory.
char* getcwd(char* buffer, size_t size) {
    static const char kCwd[] = WOWEE_CONSOLE_DATA_ROOT;
    if (!buffer || size < sizeof(kCwd)) {
        errno = buffer ? ERANGE : EINVAL;
        return nullptr;
    }
    std::memcpy(buffer, kCwd, sizeof(kCwd));
    return buffer;
}

// Nothing the client reads is a symbolic link.
ssize_t readlink(const char* path, char* buffer, size_t size) {
    (void)path; (void)buffer; (void)size;
    errno = EINVAL;
    return -1;
}

int symlink(const char* target, const char* path) {
    (void)target; (void)path;
    errno = EPERM;
    return -1;
}

int link(const char* target, const char* path) {
    (void)target; (void)path;
    errno = EPERM;
    return -1;
}

pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

}  // extern "C"
