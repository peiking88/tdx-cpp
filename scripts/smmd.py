#!/usr/bin/env python3
"""SMMD — Stock Market Manipulation Detection (Zaman et al., 206).

论文: "A Clustering-Based Framework for Identifying Suspicious Trading
Patterns in Capital Market" (arXiv:2607.04184).

流水线:
  1. 从 TDengine 拉取日线 OHLCV+amount
  2. 特征工程 (30 日滚动 → 8 个特征)
  3. StandardScaler
  4. K-Means++ (k=5, elbow 选)
  5. 距离 + 行为阈值 → 异常标记
  6. 欺诈类型启发式分类
  7. 嫌疑评分 + 风险分级
  8. Silhouette 评估 + 可视化

适配说明:
  - 论文用 DSE 数据含 "Trades" (笔数), TDengine 日线无此字段,
    故 ST (Trades/T30) 用 volume 代理, 实际用 7 个特征聚类.
  - 论文阈值 Δpthr=10% 针对 DSE 高波动市场, A 股默认 7%.
"""

import argparse
import os
import sys
from collections import defaultdict
from datetime import datetime

import numpy as np
import pandas as pd
import taosws
from common import (parse_code, zxg_codes, all_mainboard_codes,
                    apply_qfq, batch_fetch_adjust)

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------
TDENGINE_URL = os.environ.get("TDENGINE_URL", "taosws://root:taosdata@localhost:6041")

FEATURE_COLS = ["dPct", "range", "sigma30", "SV", "STurn", "VWAP", "Pavg"]


# ---------------------------------------------------------------------------
# 数据获取
# ---------------------------------------------------------------------------
def connect():
    return taosws.connect(TDENGINE_URL)


def fetch_kline(conn, market, code, days=365 * 12):
    """拉取单只股票日线, 返回 DataFrame[ts,O,H,L,C,V,amount]."""
    tbl = f"tdx.k_{market}{code}_1d"
    try:
        r = conn.query(
            f"SELECT ts, open, high, low, close, volume, amount "
            f"FROM {tbl} WHERE ts > NOW() - {days}d ORDER BY ts"
        )
    except Exception as e:
        return None
    rows = list(r)
    if not rows:
        return None
    df = pd.DataFrame(rows, columns=["ts", "O", "H", "L", "C", "V", "amount"])
    df["ts"] = pd.to_datetime(df["ts"])
    for c in ("O", "H", "L", "C", "V", "amount"):
        df[c] = pd.to_numeric(df[c], errors="coerce")
    df = df.dropna().reset_index(drop=True)
    return df if len(df) >= 60 else None


# ---------------------------------------------------------------------------
# 特征工程
# ---------------------------------------------------------------------------
def engineer(df):
    """由日线 DataFrame 计算特征矩阵. 追加列到 df 上."""
    df = df.copy()
    df["dPct"] = (df["C"] - df["O"]) / df["O"] * 100
    df["range"] = df["H"] - df["L"]
    df["sigma30"] = df["C"].rolling(30, min_periods=15).std()
    df["V30"] = df["V"].rolling(30, min_periods=15).mean()
    df["Turn30"] = df["amount"].rolling(30, min_periods=15).mean()
    df["SV"] = df["V"] / df["V30"]
    df["STurn"] = df["amount"] / df["Turn30"]
    df["VWAP"] = df["amount"] / df["V"].replace(0, np.nan)
    df["Pavg"] = (df["O"] + df["C"]) / 2
    return df


# ---------------------------------------------------------------------------
# 聚类 + 异常检测
# ---------------------------------------------------------------------------
def cluster_and_detect(all_features, k=5):
    """输入: list of (market, code, df_featured).
    返回: dict (market, code) -> df (含 cluster, dist, suspicious, fraud_type).
    """
    from sklearn.preprocessing import StandardScaler
    from sklearn.cluster import KMeans

    # 拼接所有股票
    big = []
    for market, code, df in all_features:
        d = df[FEATURE_COLS].copy()
        d["_market"] = market
        d["_code"] = code
        big.append(d)
    if not big:
        return {}
    X = pd.concat(big, ignore_index=True)
    idx = X[["_market", "_code"]]
    X = X[FEATURE_COLS].replace([np.inf, -np.inf], np.nan).fillna(0)

    scaler = StandardScaler()
    Xs = scaler.fit_transform(X)

    km = KMeans(n_clusters=k, init="k-means++", n_init=10, random_state=42)
    labels = km.fit_predict(Xs)

    # 距离
    centers = km.cluster_centers_
    dists = np.linalg.norm(Xs - centers[labels], axis=1)

    # 阈值
    td = np.percentile(dists, 95)
    tSV = np.percentile(X["SV"].values, 95)

    # 异常标记: 距离>p95 且 (|dPct|>7 或 SV>p95)
    dPct = X["dPct"].values
    SV = X["SV"].values
    suspicious = (dists > td) & ((np.abs(dPct) > 7) | (SV > tSV))

    idx = idx.copy()
    idx["cluster"] = labels
    idx["dist"] = dists
    idx["suspicious"] = suspicious

    # 分回各股票
    out = {}
    for (m, c), g in idx.groupby(["_market", "_code"]):
        df = all_features_dict[(m, c)].copy()
        g = g.reset_index(drop=True)
        for col in ("cluster", "dist", "suspicious"):
            df[col] = g[col].values
        out[(m, c)] = df
    return out


# ---------------------------------------------------------------------------
# 欺诈分类
# ---------------------------------------------------------------------------
def classify_fraud(df):
    """对 suspicious=True 的行打 fraud_type 标签."""
    df = df.copy()
    df["fraud_type"] = "normal"

    sus = df["suspicious"]
    dPct = df["dPct"]
    SV = df["SV"]
    STurn = df["STurn"]
    range_ = df["range"]
    range_p90 = df["range"].quantile(0.90) if len(df) > 30 else np.inf

    # Pump & Dump: +10%↑ 后 5 日内 -10%↓, 量 spike
    # 简化: 当日 dPct>10 且 SV>p95 → 标记, 5 日内跌幅确认
    p95_SV = df["SV"].quantile(0.95)
    mask_pump = sus & (dPct > 10) & (SV > p95_SV)
    # 5 日 lookahead: 检查未来 5 日是否跌回 -10%
    if mask_pump.any():
        close = df["C"].values
        flagged_idx = df.index[mask_pump]
        for i in flagged_idx:
            cur = df.index.get_loc(i)
            future = close[cur + 1: cur + 6]
            if len(future) and (future.min() - close[cur]) / close[cur] * 100 < -10:
                df.loc[i, "fraud_type"] = "Pump & Dump"

    # Spoofing: 价波动 <5%, 量+成交额双 spike
    mask_spoof = (sus & (df["fraud_type"] == "normal") &
                  (dPct.abs() < 5) & (SV > p95_SV) &
                  (STurn > df["STurn"].quantile(0.95)))
    df.loc[mask_spoof, "fraud_type"] = "Spoofing"

    # Rug Pull: +10%↑ 后 5 日内量萎缩 <0.5× 且价跌
    mask_rug = (sus & (df["fraud_type"] == "normal") &
                (dPct > 10) & (SV > p95_SV))
    if mask_rug.any():
        V = df["V"].values
        close = df["C"].values
        flagged_idx = df.index[mask_rug]
        for i in flagged_idx:
            cur = df.index.get_loc(i)
            future_v = V[cur + 1: cur + 6]
            future_c = close[cur + 1: cur + 6]
            if (len(future_v) and future_v.mean() < 0.5 * V[cur] and
                    len(future_c) and (future_c.min() - close[cur]) / close[cur] * 100 < -10):
                df.loc[i, "fraud_type"] = "Rug Pull"

    # Insider Trading: dPct>10, 量 spike <1.0× (低量异动)
    mask_insider = (sus & (df["fraud_type"] == "normal") &
                    (dPct > 10) & (SV < 1.0))
    df.loc[mask_insider, "fraud_type"] = "Insider Trading"

    # Fake Breakout: dPct>10 后 5 日反转, range>p90
    mask_fake = (sus & (df["fraud_type"] == "normal") &
                 (dPct > 10) & (range_ > range_p90))
    if mask_fake.any():
        close = df["C"].values
        flagged_idx = df.index[mask_fake]
        for i in flagged_idx:
            cur = df.index.get_loc(i)
            future = close[cur + 1: cur + 6]
            if len(future) and (future.max() - close[cur]) / close[cur] * 100 < -5:
                df.loc[i, "fraud_type"] = "Fake Breakout"

    # Unclassified: 仍 suspicious 但未命中以上
    mask_unclass = sus & (df["fraud_type"] == "normal")
    df.loc[mask_unclass, "fraud_type"] = "Unclassified"

    return df


# ---------------------------------------------------------------------------
# 评分 + 风险分级
# ---------------------------------------------------------------------------
def score_and_risk(results):
    """计算每只股票的 suspicion score 和 risk level."""
    scores = []
    for (market, code), df in results.items():
        total = len(df)
        flagged = df["suspicious"].sum()
        if flagged == 0:
            score = 0.0
        else:
            freq = flagged / total
            pr_dist = df.loc[df["suspicious"], "dist"].mean()
            pr = (df["dist"] <= pr_dist).mean()  # percentile rank
            score = 0.6 * freq * 100 + 0.4 * pr * 100
        scores.append({"market": market, "code": code,
                       "total": total, "flagged": int(flagged),
                       "score": round(score, 2)})
    sc = pd.DataFrame(scores)
    if sc.empty:
        return sc
    # 风险分级 (基于 score 百分位)
    sc["pct"] = sc["score"].rank(pct=True) * 100
    sc["risk"] = pd.cut(sc["pct"],
                        bins=[-1, 25, 50, 75, 90, 101],
                        labels=["Minimal", "Low", "Medium", "High", "Critical"])
    return sc.sort_values("score", ascending=False).reset_index(drop=True)


# ---------------------------------------------------------------------------
# 评估
# ---------------------------------------------------------------------------
def evaluate(results):
    """Silhouette score (采样 ≤10k)."""
    from sklearn.metrics import silhouette_score
    rows = []
    for (m, c), df in results.items():
        sub = df[FEATURE_COLS].replace([np.inf, -np.inf], np.nan).fillna(0)
        sub["cluster"] = df["cluster"].values
        sub = sub.dropna()
        if len(sub) > 2 and sub["cluster"].nunique() > 1:
            rows.append(sub)
    if not rows:
        return None
    X = pd.concat(rows)
    labels = X["cluster"].values
    X = X.drop(columns=["cluster"])
    if len(X) > 10000:
        idx = np.random.choice(len(X), 10000, replace=False)
        X, labels = X.iloc[idx], labels[idx]
    return round(silhouette_score(X, labels), 4)


# ---------------------------------------------------------------------------
# 可视化
# ---------------------------------------------------------------------------
def plot_suspicious(results, top_n=6, output_dir=None):
    """绘制 top_n 只嫌疑股的收盘价 + 红×标记."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # 按 flagged 数排序取 top
    ranked = sorted(results.items(),
                    key=lambda kv: kv[1]["suspicious"].sum(), reverse=True)[:top_n]
    if not ranked:
        return

    n = len(ranked)
    fig, axes = plt.subplots(n, 1, figsize=(14, 3 * n), squeeze=False)
    for ax, ((m, c), df) in zip(axes.flat, ranked):
        ax.plot(df["ts"], df["C"], linewidth=0.8, color="steelblue")
        sus = df[df["suspicious"]]
        if len(sus):
            ax.scatter(sus["ts"], sus["C"], c="red", marker="x",
                       s=40, zorder=5, label=f"sus={len(sus)}")
        ax.set_title(f"{m}{c}  flagged={len(sus)}/{len(df)}")
        ax.legend(loc="upper left", fontsize=8)
        ax.grid(alpha=0.3)
    plt.tight_layout()
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
        p = os.path.join(output_dir, "suspicious_top.png")
        plt.savefig(p, dpi=100)
        print(f"[plot] saved {p}")
    plt.close()


def plot_fraud_distribution(scores, results, output_dir=None):
    """欺诈类型分布饼图."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fraud_counts = defaultdict(int)
    for (m, c), df in results.items():
        for ft in df.loc[df["suspicious"], "fraud_type"]:
            fraud_counts[ft] += 1
    if not fraud_counts:
        return
    labels = list(fraud_counts.keys())
    sizes = list(fraud_counts.values())
    plt.figure(figsize=(8, 6))
    plt.pie(sizes, labels=labels, autopct="%1.1f%%", startangle=90)
    plt.title("Fraud Type Distribution")
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
        p = os.path.join(output_dir, "fraud_dist.png")
        plt.savefig(p, dpi=100)
        print(f"[plot] saved {p}")
    plt.close()


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="SMMD — 股市操纵检测")
    ap.add_argument("--codes", nargs="*", help="指定代码, 如 sh600000 sz000001")
    ap.add_argument("--zxg", action="store_true", help="用自选股 zxg.blk")
    ap.add_argument("--all", action="store_true", help="全主板")
    ap.add_argument("--days", type=int, default=365 * 10, help="回溯天数")
    ap.add_argument("--k", type=int, default=5, help="聚类数")
    ap.add_argument("--limit", type=int, help="最多分析多少只 (调试用)")
    ap.add_argument("--output-dir", default="output/smmd", help="输出目录")
    ap.add_argument("--no-plot", action="store_true", help="不画图")
    ap.add_argument("--top-n", type=int, default=6, help="画图 top N")
    args = ap.parse_args()

    conn = connect()

    # 标的池 → list of (market, code) tuples
    if args.codes:
        pool = [parse_code(c) for c in args.codes]
    elif args.zxg:
        pool = [parse_code(c) for c in zxg_codes()]
    elif args.all:
        pool = all_mainboard_codes(conn)
    else:
        pool = [parse_code(c) for c in zxg_codes()]
    if not pool:
        print("[error] empty pool", file=sys.stderr)
        sys.exit(1)
    if args.limit:
        pool = pool[: args.limit]

    print(f"[pool] {len(pool)} stocks")

    # 拉数据 + 前复权 + 特征
    adj_by_mc = batch_fetch_adjust(conn, pool)
    print(f"[fetch] {len(adj_by_mc)} 只有除权事件 (应用前复权)")
    all_features = []
    for i, (m, c) in enumerate(pool):
        df = fetch_kline(conn, m, c, days=args.days)
        if df is None:
            continue
        df = apply_qfq(df, adj_by_mc.get((m, c)))
        df = engineer(df)
        all_features.append((m, c, df))
        if (i + 1) % 50 == 0:
            print(f"  fetched {i + 1}/{len(pool)}")
    print(f"[fetch] {len(all_features)} stocks with data")
    if not all_features:
        print("[error] no data", file=sys.stderr)
        sys.exit(1)

    # 全局字典 (给 classify 用)
    global all_features_dict
    all_features_dict = {(m, c): df for m, c, df in all_features}

    # 聚类 + 异常检测
    print(f"[cluster] K-Means++ k={args.k}")
    results = cluster_and_detect(all_features, k=args.k)

    # 欺诈分类
    for key in results:
        results[key] = classify_fraud(results[key])

    # 评分
    scores = score_and_risk(results)

    # 评估
    sil = evaluate(results)
    print(f"[eval] Silhouette = {sil}")

    # 汇总
    total_trades = sum(len(df) for df in results.values())
    total_sus = sum(df["suspicious"].sum() for df in results.values())
    print(f"[summary] {total_sus}/{total_trades} suspicious "
          f"({total_sus / total_trades * 100:.2f}%)")

    # 输出
    os.makedirs(args.output_dir, exist_ok=True)
    scores.to_csv(os.path.join(args.output_dir, "scores.csv"), index=False)
    # 所有 suspicious 明细
    sus_rows = []
    for (m, c), df in results.items():
        sub = df[df["suspicious"]][["ts", "O", "H", "L", "C", "V",
                                    "dPct", "SV", "dist", "fraud_type"]].copy()
        sub["market"] = m
        sub["code"] = c
        sus_rows.append(sub)
    if sus_rows:
        pd.concat(sus_rows).to_csv(os.path.join(args.output_dir,
                                               "suspicious_trades.csv"),
                                  index=False)
    print(f"[output] → {args.output_dir}/")

    # 打印 top 20
    print("\n=== Top 20 Suspicious Symbols ===")
    print(scores.head(20).to_string(index=False))

    # 欺诈分布
    fraud_counts = defaultdict(int)
    for (m, c), df in results.items():
        for ft in df.loc[df["suspicious"], "fraud_type"]:
            fraud_counts[ft] += 1
    print("\n=== Fraud Type Distribution ===")
    for ft, n in sorted(fraud_counts.items(), key=lambda x: -x[1]):
        print(f"  {ft:20s} {n:6d} ({n / total_sus * 100:.1f}%)")

    # 画图
    if not args.no_plot:
        plot_suspicious(results, top_n=args.top_n, output_dir=args.output_dir)
        plot_fraud_distribution(scores, results, output_dir=args.output_dir)


if __name__ == "__main__":
    main()
