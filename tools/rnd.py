#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rnd.py —— 用板子 ADC 底噪生成随机 hex（娱乐级，勿用于密钥）

用法：
  python3 rnd.py                          # 8位hex（32bit），约2-3秒
  python3 rnd.py --bytes 8               # 16位hex
  python3 rnd.py --url http://192.168.0.179   # 局域网直连更快
"""
import argparse
import urllib.request
import urllib.parse


def read_adc(url, pw):
    q = urllib.parse.urlencode({"pw": pw})
    with urllib.request.urlopen("%s/adc?%s" % (url, q), timeout=10) as r:
        return int(r.read().decode().strip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://47.99.112.251:7000")
    ap.add_argument("--pw", default="m61pin")
    ap.add_argument("--bytes", type=int, default=4, help="输出字节数（4=8位hex）")
    a = ap.parse_args()

    bits = a.bytes * 8
    acc = 0
    for i in range(bits):
        v = read_adc(a.url, a.pw)
        acc = ((acc << 1) | (v & 1)) & ((1 << bits) - 1)  # 取最低位拼装
    print("%0*x" % (a.bytes * 2, acc))


if __name__ == "__main__":
    main()
