#include <libptrace/system.hpp>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

using std::string_view;

struct SyscallInfo 
{
    int argCount;
    string_view name;
};

static const SyscallInfo syscalls[] = {
    #include "syscalls.inc" // generated this bad boi with a script
};

string_view get_syscall_name(int syscall) 
{
    if (syscall < 0 || syscall >= ARRAY_SIZE(syscalls)) 
    {
        return "?????";
    }
    return syscalls[syscall].name;
}

int get_syscall_arg_count(int syscall) 
{
    if (0 <= syscall && syscall <= ARRAY_SIZE(syscalls))
    {
        return syscalls[syscall].argCount;
    }
    return -1;
}
