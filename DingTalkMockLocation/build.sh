#!/bin/bash
# 构建脚本 - 使用系统Gradle构建APK
# 要求：已安装 JDK 17+ 和 Android SDK（需设置 ANDROID_HOME 或 ANDROID_SDK_ROOT）

set -e

cd "$(dirname "$0")"

echo "============================================"
echo "  钉钉虚拟打卡 - Android 构建脚本"
echo "============================================"

# 检查 JDK
if ! command -v java &> /dev/null; then
    echo "[错误] 未检测到 Java，请安装 JDK 17+"
    exit 1
fi

# 检查 Android SDK
if [ -z "$ANDROID_HOME" ] && [ -z "$ANDROID_SDK_ROOT" ]; then
    echo "[警告] 未设置 ANDROID_HOME 或 ANDROID_SDK_ROOT 环境变量"
    echo "请安装 Android SDK 并设置环境变量，或使用 Android Studio 打开本项目构建。"
    echo ""
    echo "项目路径: $(pwd)"
    exit 1
fi

# 创建 local.properties
echo "sdk.dir=${ANDROID_HOME:-$ANDROID_SDK_ROOT}" > local.properties

echo "[1/2] 构建 Debug APK..."
if command -v gradle &> /dev/null; then
    gradle assembleDebug --no-daemon
else
    echo "[错误] 未检测到 gradle 命令"
    echo "请安装 Gradle 或使用 Android Studio 打开项目"
    exit 1
fi

echo ""
echo "[2/2] 构建完成！"
APK_PATH="app/build/outputs/apk/debug/app-debug.apk"
if [ -f "$APK_PATH" ]; then
    echo "APK 路径: $(pwd)/$APK_PATH"
    echo ""
    echo "安装到设备（需开启USB调试）："
    echo "  adb install -r $APK_PATH"
else
    echo "[错误] 未找到生成的 APK，请检查构建日志"
    exit 1
fi
