#!/bin/bash

set -e

echo "=========================================="
echo "  Watchdog - Server Setup"
echo "=========================================="

# 1. 检查 root 权限
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (use sudo)"
    exit 1
fi

# 2. 安装依赖
echo ""
echo "Installing dependencies..."
pip3 install pyserial || {
    echo "Failed to install pyserial. Trying apt..."
    apt-get update
    apt-get install -y python3-serial
}

# 3. 创建安装目录
echo ""
echo "Creating installation directory..."
mkdir -p /opt/usb-watchdog

# 4. 复制守护进程
echo ""
echo "Installing daemon..."
cp usb-watchdog.py /opt/usb-watchdog/
chmod +x /opt/usb-watchdog/usb-watchdog.py

# 5. 配置 udev 规则 (固定设备名)
echo ""
echo "Configuring udev rules..."
cat > /etc/udev/rules.d/99-esp32-watchdog.rules << 'EOF'
# ESP32-C3 USB Watchdog - Fixed device name
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4002", MODE="0666", SYMLINK+="ttyESP32Watchdog"
EOF

udevadm control --reload-rules
udevadm trigger

# 6. 安装 systemd 服务
echo ""
echo "Installing systemd service..."
cp usb-watchdog.service /etc/systemd/system/
systemctl daemon-reload

# 7. 加载 CDC-ACM 模块
echo ""
echo "Loading CDC-ACM module..."
modprobe cdc_acm || echo "cdc_acm may already be built-in"

# 添加到 /etc/modules 以便开机加载
if ! grep -q "cdc_acm" /etc/modules 2>/dev/null; then
    echo "cdc_acm" >> /etc/modules
fi

# 8. 启动服务
echo ""
echo "Starting service..."
systemctl enable usb-watchdog
systemctl start usb-watchdog

# 9. 检查状态
echo ""
echo "Service status:"
systemctl status usb-watchdog --no-pager

echo ""
echo "=========================================="
echo "Setup Complete!"
echo "=========================================="
echo ""
echo "Useful commands:"
echo "  Check status:  sudo systemctl status usb-watchdog"
echo "  View logs:     sudo journalctl -u usb-watchdog -f"
echo "  Restart:       sudo systemctl restart usb-watchdog"
echo "  Stop:          sudo systemctl stop usb-watchdog"
echo ""
echo "USB device should appear as: /dev/ttyESP32Watchdog (or /dev/ttyACM0)"
echo ""
