#include <windows.h>

#include <cerrno>
#include <filesystem>
#include <process.h>
#include <string>
#include <vector>

int wmain(int argc, wchar_t **argv)
{
    std::wstring executable(32768, L'\0');
    const DWORD size =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (size == 0 || size >= executable.size()) return 126;
    executable.resize(size);
    const std::filesystem::path root = std::filesystem::path(executable).parent_path();
    const std::filesystem::path python = root / L"python" / L"python.exe";
    const std::filesystem::path script = root / L"convert_pth.py";
    if (!std::filesystem::is_regular_file(python)
        || !std::filesystem::is_regular_file(script))
        return 127;

    const std::wstring python_home = (root / L"python").wstring();
    const std::wstring python_path =
        (root / L"site-packages").wstring() + L";" + root.wstring();
    SetEnvironmentVariableW(L"PYTHONHOME", python_home.c_str());
    SetEnvironmentVariableW(L"PYTHONPATH", python_path.c_str());
    SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
    SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE", L"1");
    SetEnvironmentVariableW(L"OMP_NUM_THREADS", L"1");
    SetEnvironmentVariableW(L"MKL_NUM_THREADS", L"1");

    std::vector<std::wstring> storage{python.wstring(), L"-s", script.wstring()};
    for (int index = 1; index < argc; ++index) storage.emplace_back(argv[index]);
    std::vector<const wchar_t *> arguments;
    arguments.reserve(storage.size() + 1);
    for (const std::wstring &argument : storage) arguments.push_back(argument.c_str());
    arguments.push_back(nullptr);
    _wexecv(python.c_str(), arguments.data());
    return errno == 0 ? 126 : errno;
}
