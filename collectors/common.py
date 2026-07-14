"""采集器公共工具:从结果文本里提取存在的图片文件。"""
from __future__ import annotations

import glob
import hashlib
import os
import re

IMG_EXT = ("png", "jpg", "jpeg", "webp", "bmp", "gif")
# 匹配路径样式的 token(含 * 通配、~、相对/绝对),以图片扩展名结尾
_PAT = re.compile(r"[~\w./\-*]+\.(?:" + "|".join(IMG_EXT) + r")", re.IGNORECASE)

MAX_IMAGES = 4

_MD = re.compile(r"(\*\*|__|`+|~~)")
_MD_HEAD = re.compile(r"^\s{0,3}#{1,6}\s+", re.MULTILINE)


def clean_text(s: str) -> str:
    """去掉 markdown 强调符号,让墨水屏显示更干净。"""
    if not s:
        return s
    s = _MD_HEAD.sub("", s)
    s = _MD.sub("", s)
    return s


def img_id(path: str) -> str:
    return hashlib.sha1(path.encode("utf-8")).hexdigest()[:12]


def extract_images(text: str, cwd: str | None):
    """从文本里找出真实存在的图片文件,返回 [{id,name,path}]。支持 glob 展开。"""
    if not text:
        return []
    found = []
    seen = set()
    for m in _PAT.findall(text):
        token = m
        cands = []
        # 绝对/家目录
        for base in ([token] if os.path.isabs(token) or token.startswith("~")
                     else ([os.path.join(cwd, token)] if cwd else []) + [token]):
            base = os.path.expanduser(base)
            if any(c in base for c in "*?[]"):
                cands.extend(sorted(glob.glob(base)))
            else:
                cands.append(base)
        for p in cands:
            rp = os.path.realpath(p)
            if rp in seen:
                continue
            if os.path.isfile(rp) and rp.split(".")[-1].lower() in IMG_EXT:
                seen.add(rp)
                found.append({"id": img_id(rp), "name": os.path.basename(rp), "path": rp})
                if len(found) >= MAX_IMAGES:
                    return found
    return found
