#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OneNET OTA 设备侧流程 —— PC 侧对标工具（M1）

用途：在**不碰板子**的前提下，把整条 OTA 链路在 PC 上走一遍，
确认平台侧配置全对，并产出 MCU 端 C 代码的对照基准。

已实测确认（2026-10-07）：
    Authorization: version=2022-05-01
                 & res=products/{pro_id}/devices/{dev_name}
                 & et={过期时间戳}
                 & method=sha1
                 & sign={base64(HMAC-SHA1(base64decode(accessKey), sfs))}

    sfs = et + "\n" + method + "\n" + res + "\n" + version   <- 字母序，换行分隔

用法：
    cp onenet_cfg.json.example onenet_cfg.json   # 填自己的密钥，该文件应 gitignore
    python onenet_ota.py version                      # 上报当前版本
    python onenet_ota.py check                        # 检测升级任务
    python onenet_ota.py download --tid 12 --out fw.bin   # 分片下载
    python onenet_ota.py status --tid 12 --step 100   # 上报进度/状态
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(HERE, "onenet_cfg.json")

# 与 MCU 端常量保持一致：SOTA(MCU 软件) = 2
OTA_TYPE_SOTA = 2
TOKEN_VERSION = "2022-05-01"
TOKEN_METHOD = "sha1"

# 上报状态码（POST status 的 step > 100 时）
STATUS = {
    101: "升级包下载成功", 102: "下载失败-空间不足", 103: "下载失败-内存溢出",
    104: "下载失败-请求超时", 105: "下载失败-电量不足", 106: "下载失败-信号不良",
    107: "下载失败-未知异常", 201: "升级成功", 202: "升级失败-电量不足",
    203: "升级失败-内存溢出", 204: "升级失败-目标版本不一致", 205: "升级失败-MD5校验失败",
    206: "升级失败-未知异常", 207: "升级失败-达到最大重试次数",
}


def load_cfg():
    if not os.path.exists(CFG_PATH):
        sys.exit("找不到 %s\n请先: cp onenet_cfg.json.example onenet_cfg.json 并填入 access_key"
                 % CFG_PATH)
    with open(CFG_PATH, encoding="utf-8") as f:
        cfg = json.load(f)
    for k in ("access_key", "pro_id", "dev_name"):
        if not cfg.get(k) or str(cfg[k]).startswith("在这里"):
            sys.exit("onenet_cfg.json 里的 %s 还没填" % k)
    cfg.setdefault("api_base", "http://iot-api.heclouds.com/fuse-ota")
    return cfg


def build_auth(cfg, ttl=3600 * 24 * 365):
    """构造 Authorization 头。res 用设备级资源——实测产品级会被拒。"""
    res = "products/%s/devices/%s" % (cfg["pro_id"], cfg["dev_name"])
    et = int(time.time()) + ttl
    sfs = "%d\n%s\n%s\n%s" % (et, TOKEN_METHOD, res, TOKEN_VERSION)
    key = base64.b64decode(cfg["access_key"])
    sig = base64.b64encode(
        hmac.new(key, sfs.encode("utf-8"), hashlib.sha1).digest()).decode()
    return "version=%s&res=%s&et=%d&method=%s&sign=%s" % (
        TOKEN_VERSION, urllib.parse.quote(res, safe=""), et,
        TOKEN_METHOD, urllib.parse.quote(sig, safe=""))


def http(method, url, cfg, body=None, extra_headers=None, raw=False):
    """返回 (status, headers, body)。raw=True 时 body 是 bytes（下载固件用）。"""
    data = json.dumps(body).encode("utf-8") if body is not None else None
    headers = {"Authorization": build_auth(cfg)}
    if data:
        headers["Content-Type"] = "application/json"
    headers.update(extra_headers or {})
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, dict(r.headers), (r.read() if raw else r.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers or {}), (e.read() if raw else e.read().decode("utf-8", "replace"))


def cmd_version(cfg, args):
    """上报设备版本号。升级包目标版本会与 s_version 比对。"""
    url = "%s/%s/%s/version" % (cfg["api_base"], cfg["pro_id"], cfg["dev_name"])
    code, _, resp = http("POST", url, cfg,
                         {"s_version": args.s_version, "f_version": args.f_version})
    print("[上报版本] s_version=%s f_version=%s" % (args.s_version, args.f_version))
    print("  HTTP %s -> %s" % (code, resp.strip()))
    return code, resp


def cmd_query_version(cfg, args):
    url = "%s/%s/%s/version" % (cfg["api_base"], cfg["pro_id"], cfg["dev_name"])
    code, _, resp = http("GET", url, cfg)
    print("[查询版本] HTTP %s -> %s" % (code, resp.strip()))
    return code, resp


def cmd_check(cfg, args):
    """检测升级任务。type=2 是 SOTA(MCU 软件)。"""
    url = "%s/%s/%s/check?type=%d&version=%s" % (
        cfg["api_base"], cfg["pro_id"], cfg["dev_name"], args.type, args.version)
    code, _, resp = http("GET", url, cfg)
    print("[检测任务] HTTP %s -> %s" % (code, resp.strip()))
    try:
        d = json.loads(resp)
    except Exception:
        return code, resp
    if d.get("code") == 0:
        t = d.get("data") or {}
        print("  >>> 有升级任务: 目标版本=%s tid=%s 大小=%s md5=%s" %
              (t.get("target"), t.get("tid"), t.get("size"), t.get("md5")))
    else:
        print("  >>> 无任务或失败: code=%s msg=%s" % (d.get("code"), d.get("msg")))
    return code, resp


def cmd_check_status(cfg, args):
    """检测升级状态（按 tid）。用来观察上报前后状态机怎么迁移。"""
    url = "%s/%s/%s/%s/check" % (
        cfg["api_base"], cfg["pro_id"], cfg["dev_name"], args.tid)
    code, _, resp = http("GET", url, cfg)
    names = {1: "待升级", 2: "下载中", 3: "升级中", 4: "升级成功", 5: "升级失败", 6: "升级取消"}
    print("[检测状态] HTTP %s -> %s" % (code, resp.strip()))
    try:
        st = (json.loads(resp).get("data") or {}).get("status")
        if st is not None:
            print("  >>> status=%s (%s)" % (st, names.get(st, "未知")))
    except Exception:
        pass
    return code, resp


def cmd_download(cfg, args):
    """分片下载。平台原生支持 Range；Ota-Errno 在响应头里，必须检查。"""
    url = "%s/%s/%s/%s/download" % (
        cfg["api_base"], cfg["pro_id"], cfg["dev_name"], args.tid)
    hdr = {}
    if args.start is not None:
        hdr["Range"] = "%s-%s" % (args.start, args.end if args.end is not None else "")
    code, headers, blob = http("GET", url, cfg, extra_headers=hdr, raw=True)
    # 头是小写化的，统一处理
    h = {k.lower(): v for k, v in headers.items()}
    print("[下载] HTTP %s" % code)
    print("  Content-Range      : %s" % h.get("content-range"))
    print("  Content-Length     : %s" % h.get("content-length"))
    print("  Content-Disposition: %s" % h.get("content-disposition"))
    print("  Ota-Errno          : %s   <- 0 才是成功，非 0 要在这里就拦" % h.get("ota-errno"))
    if not isinstance(blob, bytes):
        print("  body: %s" % str(blob)[:200])
        return code, None
    print("  收到 %d 字节" % len(blob))
    if args.out:
        with open(args.out, "wb") as f:
            f.write(blob)
        print("  已写入 %s" % args.out)
    return code, blob


def cmd_status(cfg, args):
    """上报进度(0~100)或状态码(>100)。"""
    url = "%s/%s/%s/%s/status" % (
        cfg["api_base"], cfg["pro_id"], cfg["dev_name"], args.tid)
    code, _, resp = http("POST", url, cfg, {"step": args.step})
    note = STATUS.get(args.step, "下载进度 %d%%" % args.step if args.step <= 100 else "未知码")
    print("[上报状态] step=%s (%s)" % (args.step, note))
    print("  HTTP %s -> %s" % (code, resp.strip()))
    return code, resp


def main():
    p = argparse.ArgumentParser(description="OneNET OTA PC 侧对标工具")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("version", help="上报设备版本号")
    s.add_argument("--s-version", default="1.0.0", help="应用软件版本(与升级包目标版本比对)")
    s.add_argument("--f-version", default="1.0.0", help="模组版本")
    s.set_defaults(func=cmd_version)

    s = sub.add_parser("query-version", help="查询平台记录的版本号")
    s.set_defaults(func=cmd_query_version)

    s = sub.add_parser("check", help="检测升级任务")
    s.add_argument("--type", type=int, default=OTA_TYPE_SOTA, help="1=FOTA 2=SOTA(默认)")
    s.add_argument("--version", default="1.0.0", help="设备当前版本")
    s.set_defaults(func=cmd_check)

    s = sub.add_parser("check-status", help="检测升级状态(按任务 ID)")
    s.add_argument("--tid", required=True, help="任务 ID")
    s.set_defaults(func=cmd_check_status)

    s = sub.add_parser("download", help="下载升级包(支持 Range 分片)")
    s.add_argument("--tid", required=True, help="任务 ID")
    s.add_argument("--start", type=int, default=None, help="Range 起始字节(含)")
    s.add_argument("--end", type=int, default=None, help="Range 结束字节(含)")
    s.add_argument("--out", default=None, help="保存路径")
    s.set_defaults(func=cmd_download)

    s = sub.add_parser("status", help="上报进度/状态")
    s.add_argument("--tid", required=True, help="任务 ID")
    s.add_argument("--step", type=int, required=True, help="0~100 进度，>100 状态码")
    s.set_defaults(func=cmd_status)

    args = p.parse_args()
    cfg = load_cfg()
    args.func(cfg, args)


if __name__ == "__main__":
    main()
