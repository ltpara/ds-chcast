// Console entry point of chast-service.exe: SCM control, machine settings and
// the internal worker mode. All long-running work happens in service.cc and
// worker.cc; this file only parses the command line and reports errors.

#include <windows.h>

#include <cstdio>
#include <string>

#include "chast/host_settings.h"
#include "service.h"

namespace {

void print_usage() {
    std::printf(
        "chast-service — chast 主机服务\n\n"
        "  --install     创建并启动服务，写入机器设置和防火墙规则（需要管理员）\n"
        "  --uninstall   停止并删除服务，移除防火墙规则（需要管理员）\n"
        "  --token       打印机器设置中的当前访问令牌\n"
        "  --run         SCM 入口，由服务管理器调用\n"
        "  --worker      会话工作进程，由服务在用户会话中启动\n"
        "  --help        显示本帮助\n");
}

int report(int code, const std::string& error) {
    if (code != 0 && !error.empty()) {
        std::fprintf(stderr, "chast-service: %s\n", error.c_str());
    }
    return code;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string error;
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::wstring command = argv[1];
    if (command == L"--install") {
        const int code = chast::host::install_service(&error);
        if (code == 0) {
            std::printf("chast-host 服务已安装并启动。\n");
        }
        return report(code, error);
    }
    if (command == L"--uninstall") {
        const int code = chast::host::uninstall_service(&error);
        if (code == 0) {
            std::printf("chast-host 服务已停止并删除。\n");
        }
        return report(code, error);
    }
    if (command == L"--run") {
        return chast::host::run_service();
    }
    if (command == L"--worker") {
        std::string worker_error;
        const int code = chast::host::run_worker(&worker_error);
        if (code != 0 && worker_error.empty()) {
            return code;
        }
        return report(code, worker_error);
    }
    if (command == L"--token") {
        const std::string path = chast::machine_settings_path();
        if (path.empty()) {
            std::fprintf(stderr, "chast-service: 无法确定机器设置的位置。\n");
            return 1;
        }
        const chast::HostSettingsLoad loaded = chast::load_host_settings(path);
        if (!loaded.existed) {
            std::fprintf(stderr, "chast-service: 机器设置不存在，请先运行 --install：%s\n",
                         path.c_str());
            return 1;
        }
        if (loaded.settings.access_token.empty()) {
            std::fprintf(stderr, "chast-service: 机器设置中没有访问令牌：%s\n", path.c_str());
            return 1;
        }
        std::printf("%s\n", loaded.settings.access_token.c_str());
        return 0;
    }
    if (command == L"--help" || command == L"-h" || command == L"/?") {
        print_usage();
        return 0;
    }
    std::fprintf(stderr, "chast-service: 未知选项\n");
    print_usage();
    return 2;
}
