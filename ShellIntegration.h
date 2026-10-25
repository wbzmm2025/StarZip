#pragma once

#include <QString>

// 资源管理器右键菜单 / “打开方式”集成（仅 Windows，写入 HKCU，无需管理员权限）。
namespace ShellIntegration {

bool supported();
bool installed();

// 安装或修复当前用户的菜单项；exePath 为空时使用当前程序路径。
bool install(QString *error = nullptr);
bool uninstall(QString *error = nullptr);

} // namespace ShellIntegration
