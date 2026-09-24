#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
blink.py —— m61grok 远程点灯示例（零依赖，Python 3 标准库）

用法：
  # 经公网隧道（人在外面）
  python3 blink.py --pin 18
  # 在家里局域网（板子 IP）
  python3 blink.py --url http://192.168.0.179 --pin 18
  # 改闪烁速度 / 先确认引脚已配置
  python3 blink.py --pin 18 --interval 0.2
  python3 blink.py --pin 18 --once 1      # 只点亮不闪
  python3 blink.py --pin 18 --once 0      # 只熄灭

前提：管理页 GPIO 区已把该引脚配成「输出」。
"""

import argparse
import time
import urllib.request
import urllib.parse


def gpio(url, pw, pin, val):
    q = urllib.parse.urlencode({"pw": pw, "pin": pin, "set": val})
    with urllib.request.urlopen("%s/gpio?%s" % (url, q), timeout=10) as r:
        return r.read().decode().strip()


def main():
    ap = argparse.ArgumentParser(description="m61grok remote blink")
    ap.add_argument("--url", default="http://47.99.112.251:7000",
                    help="板子管理页地址（公网 7000 或局域网板子 IP）")
    ap.add_argument("--pw", default="m61pin", help="管理页密码")
    ap.add_argument("--pin", type=int, required=True, help="GPIO 引脚号")
    ap.add_argument("--interval", type=float, default=0.5, help="闪烁周期(秒)")
    ap.add_argument("--once", default=None, choices=["0", "1"],
                    help="只置一次电平后退出（1=亮 0=灭），不给则一直闪")
    args = ap.parse_args()

    if args.once is not None:
        print("pin %d -> %s (板子返回: %s)"
              % (args.pin, args.once, gpio(args.url, args.pw, args.pin, args.once)))
        return

    print("pin %d 闪烁中（%.1fs 周期），Ctrl+C 停止并熄灭..." % (args.pin, args.interval))
    try:
        while True:
            gpio(args.url, args.pw, args.pin, 1)
            time.sleep(args.interval)
            gpio(args.url, args.pw, args.pin, 0)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        gpio(args.url, args.pw, args.pin, 0)
        print("\n已熄灭，再见。")


if __name__ == "__main__":
    main()
