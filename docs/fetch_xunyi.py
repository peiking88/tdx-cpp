#!/usr/bin/env python3
"""抓取头条博主「寻一」全部文章（Playwright 滚动加载），存 JSON + MD。"""
import json
import os
import re
import sys
import time

from playwright.sync_api import sync_playwright

URL = ("https://www.toutiao.com/c/user/token/"
       "CiwR8Eom9FxqPfTsVlZyGOv1SBsjBe_R_GitTAo7Dpz-AddAxFSG_Y_U5wTshxpJCjw"
       "AAAAAAAAAAAAAUMyaLTTqM1DyGgutQtgE0f8E6EWmKo2SsX3nfDCJZaBLF-23B6eWza"
       "Ip1Xyk8t0TzuQQ3_eZDhjDxYPqBCIBA-iiI6M=/")
OUT_DIR = os.path.dirname(os.path.abspath(__file__))
MAX_SCROLL = 200          # 最多滚动次数
IDLE_STOP = 8             # 连续 N 次滚动无新卡片则停

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")


def parse_time(text):
    text = text.strip()
    from datetime import date, timedelta
    now = date.today()
    if "分钟前" in text or "小时前" in text or text == "刚刚":
        return now.strftime("%Y-%m-%d")
    if "昨天" in text:
        return (now - timedelta(days=1)).strftime("%Y-%m-%d")
    m = re.search(r"(\d+)天前", text)
    if m:
        return (now - timedelta(days=int(m.group(1)))).strftime("%Y-%m-%d")
    m = re.search(r"(?<![\d年])(\d{4})年(\d{1,2})月(\d{1,2})日", text)
    if m:
        return f"{m.group(1)}-{int(m.group(2)):02d}-{int(m.group(3)):02d}"
    m = re.search(r"(?<![\d年])(\d{1,2})月(\d{1,2})日", text)
    if m:
        return f"{now.year}-{int(m.group(1)):02d}-{int(m.group(2)):02d}"
    m = re.search(r"(\d{4})-(\d{2})-(\d{2})", text)
    if m:
        return m.group(0)
    return ""


def main():
    with sync_playwright() as pw:
        browser = pw.chromium.launch(headless=True)
        page = browser.new_page(user_agent=UA)
        page.goto(URL, wait_until="domcontentloaded", timeout=30000)

        # 等待首批卡片
        for sel in (".feed-card-wtt-wrapper", ".feed-card-wrapper.feed-card-article-wrapper"):
            try:
                page.wait_for_selector(sel, timeout=10000)
                break
            except Exception:
                continue

        # 滚动加载全部
        prev_n = 0
        idle = 0
        for i in range(MAX_SCROLL):
            page.mouse.wheel(0, 4000)
            page.wait_for_timeout(1200)
            n = page.evaluate(
                "document.querySelectorAll('.feed-card-wtt-wrapper,.feed-card-wrapper.feed-card-article-wrapper').length")
            print(f"scroll {i+1}: {n} cards", file=sys.stderr)
            if n == prev_n:
                idle += 1
                if idle >= IDLE_STOP:
                    break
            else:
                idle = 0
                prev_n = n

        cards = page.query_selector_all(
            ".feed-card-wtt-wrapper,.feed-card-wrapper.feed-card-article-wrapper")
        posts = []
        for card in cards:
            content_el = card.query_selector("p.content") or card.query_selector("a.title")
            time_el = card.query_selector(".time") or card.query_selector(
                ".feed-card-footer-time-cmp")
            link_el = card.query_selector("a[href*='/article/']") or card.query_selector("a.title")
            if not content_el:
                continue
            text = content_el.inner_text().strip()
            if not text:
                continue
            href = link_el.get_attribute("href") if link_el else ""
            if href and href.startswith("/"):
                href = "https://www.toutiao.com" + href
            posts.append({
                "date": parse_time(time_el.inner_text() if time_el else ""),
                "time_raw": time_el.inner_text().strip() if time_el else "",
                "url": href,
                "text": text,
            })

        # 博主名
        name = ""
        try:
            name_el = page.query_selector(".name") or page.query_selector("h1")
            name = name_el.inner_text().strip() if name_el else ""
        except Exception:
            pass
        print(f"blogger: {name!r}, posts: {len(posts)}", file=sys.stderr)
        browser.close()

    posts.sort(key=lambda p: p["date"], reverse=True)
    with open(os.path.join(OUT_DIR, "xunyi_posts.json"), "w", encoding="utf-8") as f:
        json.dump({"blogger": name, "posts": posts}, f, ensure_ascii=False, indent=1)
    # 同步写 MD 便于阅读
    with open(os.path.join(OUT_DIR, "xunyi_posts.md"), "w", encoding="utf-8") as f:
        for i, p in enumerate(posts):
            f.write(f"\n## [{i}] {p['date']} ({p['time_raw']})\n{p['url']}\n\n{p['text']}\n")
    print(f"saved {len(posts)} posts → xunyi_posts.json/md")


if __name__ == "__main__":
    main()
