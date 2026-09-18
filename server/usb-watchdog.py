import os
import sys
import time
import signal
import argparse
import logging
import serial

# 协议定义 (与 ESP32 端 usb_device.c 一致)：纯文本，行结束符为 \n
CMD_HEARTBEAT = "ZAIMA"        # ESP32 发来的心跳
CMD_ACK       = "HEI-ZAIDE"    # 服务器回复的应答
CMD_BYE       = "BYE"          # 服务器退出前通知

class USBWatchdogDaemon:
    """USB 看门狗守护进程 (纯文本协议，服务器端仅负责响应)"""

    def __init__(self, device="/dev/ttyACM0", baudrate=115200, verbose=False):
        self.device = device
        self.baudrate = baudrate
        self.running = True
        self.ser = None                    # 串口对象作为实例变量
        self.last_heartbeat = 0.0          # 仅用于日志记录，不做超时判断
        self._pending_line = b""           # 行缓冲: 解决 readline 超时读到被截断的半行问题

        logging.basicConfig(
            level=logging.DEBUG if verbose else logging.INFO,
            format='%(asctime)s [%(levelname)s] %(message)s'
        )
        self.logger = logging.getLogger("watchdog")

    def send_line(self, line):
        """发送一行文本（自动补 \n），内置串口状态检查与部分写重试"""
        if not self.ser or not self.ser.is_open:
            self.logger.warning("Cannot send: serial not open")
            return

        if not line.endswith("\n"):
            line += "\n"

        data = line.encode("utf-8")
        written = 0
        while written < len(data):
            try:
                n = self.ser.write(data[written:])
            except Exception as e:
                self.logger.warning(f"Serial write failed: {e}")
                return
            if n is None or n < 0:
                self.logger.warning("Serial write returned error")
                return
            written += n

        self.ser.flush()
        self.logger.debug(f"Sent: {line.strip()}")

    def open_device(self):
        """打开 USB CDC-ACM 设备，带自动重连"""
        self.logger.info(f"Opening {self.device} at {self.baudrate} baud...")

        while self.running:
            try:
                self.ser = serial.Serial(
                    port=self.device,
                    baudrate=self.baudrate,
                    timeout=0.5,          # 调小 timeout，退出时响应更灵敏
                )
                self.ser.reset_input_buffer()
                self._pending_line = b""   # 重连后清空旧缓冲，避免处理上一个会话残留
                self.logger.info(f"Connected to {self.device}")
                return self.ser
            except Exception as e:
                self.logger.warning(f"Waiting for device... ({e})")
                time.sleep(2)

        # self.running 被置 False，退出重连循环
        self.ser = None
        return None

    def handle_line(self, line):
        """处理一行来自 ESP32 的文本命令"""
        line = line.strip()
        if not line:
            return

        if line == CMD_HEARTBEAT:
            self.logger.debug("Heartbeat received")
            self.last_heartbeat = time.time()
            self.send_line(CMD_ACK)
        else:
            self.logger.debug(f"Unknown line: {line}")

    def run(self):
        """主循环：按行读取，收到 ZAIMA 即回复 HEI-ZAIDE"""
        self.logger.info("=" * 50)
        self.logger.info("  ESP32-C3 USB Watchdog - Server Daemon")
        self.logger.info("  Protocol: text  (ZAIMA -> HEI-ZAIDE)")
        self.logger.info("  Mode: responsive (no timeout check)")
        self.logger.info("=" * 50)

        try:
            while self.running:
                # 确保串口已打开
                if not self.ser or not self.ser.is_open:
                    self.ser = self.open_device()
                    if not self.ser:
                        self.logger.info("Stopping: device open aborted.")
                        break

                try:
                    raw = self.ser.readline()
                    if raw:
                        # readline() 在超时前可能只读到被截断的半行(尤其是高丢包时),
                        # 用 pending_line 累积, 遇到 \n/\r 才作为完整行处理。
                        self._pending_line += raw
                        while b'\n' in self._pending_line or b'\r' in self._pending_line:
                            nl = self._pending_line.find(b'\n')
                            cr = self._pending_line.find(b'\r')
                            if cr == -1 or (nl != -1 and nl < cr):
                                idx = nl
                                delim_len = 1
                            else:
                                idx = cr
                                delim_len = 1
                            line = self._pending_line[:idx]
                            self._pending_line = self._pending_line[idx + delim_len:]
                            if line:
                                try:
                                    text = line.decode("utf-8", errors="replace").strip()
                                except Exception:
                                    text = ""
                                if text:
                                    self.handle_line(text)

                except serial.SerialException as e:
                    self.logger.error(f"Serial error: {e}, reconnecting...")
                    self._pending_line = b""
                    try:
                        self.ser.close()
                    except Exception:
                        pass
                    self.ser = None

                except Exception as e:
                    self.logger.error(f"Unexpected error: {e}")
                    time.sleep(1)
                    
        except KeyboardInterrupt:
            self.logger.info("Interrupted by user (KeyboardInterrupt)")
        finally:
            self.notify_before_exit()
            self.logger.info("Daemon stopped")

    def notify_before_exit(self):
        """退出前通知 ESP32（复用已有串口，尽力而为）"""
        if self.ser and self.ser.is_open:
            try:
                self.send_line(CMD_BYE)
                self.logger.info("Notified ESP32 before exit (BYE)")
            except Exception as e:
                self.logger.warning(f"Failed to send BYE: {e}")
            finally:
                try:
                    self.ser.close()
                except Exception:
                    pass
        else:
            self.logger.info("Serial already closed, skip BYE notification.")

    def signal_handler(self, signum, frame):
        self.logger.info(f"Received signal {signum}, shutting down...")
        self.running = False

    def install_signal_handlers(self):
        signal.signal(signal.SIGINT, self.signal_handler)
        signal.signal(signal.SIGTERM, self.signal_handler)

def main():
    parser = argparse.ArgumentParser(description="ESP32-C3 USB Watchdog Daemon")
    parser.add_argument("--device", default="/dev/ttyACM0", help="USB CDC-ACM device path")
    parser.add_argument("--baudrate", type=int, default=115200, help="Baud rate")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose logging")
    args = parser.parse_args()

    daemon = USBWatchdogDaemon(
        device=args.device,
        baudrate=args.baudrate,
        verbose=args.verbose,
    )

    daemon.install_signal_handlers()
    
    daemon.run()


if __name__ == "__main__":
    main()