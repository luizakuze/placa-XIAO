#!/usr/bin/env python3
"""
Analisa os CSVs de resultados do testbed de desempenho (Wi-Fi vs
ESP-NOW vs BLE vs Zigbee) e gera:

  1. Um resumo em texto no terminal, agrupado por tecnologia e
     cenario (RTT medio/p95, PDR medio, RSSI medio, throughput
     medio), mais uma tabela de custo de memoria por tecnologia.
  2. Graficos comparativos em PNG, salvos em resultados/graficos/:
       - rtt_vs_distancia.png   (RTT medio, com p95 como barra de
         erro, para cada tecnologia, nos cenarios de alcance - so
         com ao menos duas distancias)
       - perda_vs_distancia.png (perda % para cada tecnologia, nos
         cenarios de alcance)
       - throughput_vs_payload.png (goodput do ping-pong por tamanho de
         payload, restrito ao baseline, eixo y em log)
       - comparativo_baseline.png (barras de RTT medio por
         tecnologia e payload do nucleo comum no "baseline", com p50
         e p95 marcados)
       - jitter_16bytes_baseline.png (p95 - p50 do RTT, 16 B)
       - pdr_vs_rssi.png (Packet Delivery Ratio x RSSI medido: a
         curva PDR x RSSI de cada tecnologia, juntando todos
         os cenarios de distancia e obstaculo)
       - memoria_por_tecnologia.png (RAM consumida pela pilha de
         radio e pelo enlace, em barras empilhadas)

So usa a biblioteca padrao do Python (csv, statistics) mais
matplotlib para os graficos - sem pandas, para nao exigir instalar
uma pilha de dados inteira so para ver os resultados.

Uso
---
    python3 analisar_resultados.py
    python3 analisar_resultados.py --resultados-dir ../resultados
    python3 analisar_resultados.py --sem-graficos   # so o resumo em texto

Dependencia para os graficos: matplotlib (`pip install matplotlib`).
Sem matplotlib instalado, o script ainda funciona e imprime o resumo
em texto; so os PNGs deixam de ser gerados.
"""

import argparse
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict


def parse_args():
    here = os.path.dirname(os.path.abspath(__file__))
    default_dir = os.path.join(here, "..", "resultados")

    parser = argparse.ArgumentParser(description="Resume e plota os resultados do testbed de desempenho.")
    parser.add_argument(
        "--resultados-dir", default=default_dir,
        help="Pasta com os CSVs de resultado (padrao: desempenho/resultados/)",
    )
    parser.add_argument(
        "--sem-graficos", action="store_true",
        help="So imprime o resumo em texto, sem gerar os PNGs (nao precisa de matplotlib)",
    )
    return parser.parse_args()


def to_float(value, default=None):
    try:
        if value is None or value == "" or value.strip().upper() == "NA":
            return default
        return float(value)
    except (ValueError, AttributeError):
        return default


def common_payloads(rows):
    """Tamanhos de payload presentes em TODAS as tecnologias dos dados.

    Calculado por intersecao, em vez de fixado no codigo, porque o
    conjunto depende de quais tecnologias foram coletadas: com as quatro
    presentes o nucleo e {16, 32} (o quadro do 802.15.4 nao comporta 100
    nem 200); so com Wi-Fi, ESP-NOW e BLE, o nucleo cresce para
    {16, 32, 100, 200}.

    Comparar agregados fora desse conjunto e enganoso: a media do Wi-Fi
    incluiria as linhas de 512 e 1200 bytes, que as outras nao conseguem
    enviar, inflando RTT e throughput dele contra concorrentes que nunca
    tiveram a chance de disputar aqueles pontos.
    """
    by_tech = defaultdict(set)

    for row in rows:
        payload = to_float(row.get("payload_bytes"))
        if payload is not None:
            by_tech[row["tecnologia"]].add(payload)

    if not by_tech:
        return set()

    conjuntos = list(by_tech.values())
    comum = set(conjuntos[0])
    for outro in conjuntos[1:]:
        comum &= outro

    return comum


def pdr_of(row):
    """Packet Delivery Ratio da linha, em %.

    Usa a coluna pdr_pct quando ela existe e, quando nao existe (CSV
    gerado por uma versao anterior do firmware), deduz a partir da
    perda - as duas sempre somam 100.
    """
    pdr = to_float(row.get("pdr_pct"))
    if pdr is not None:
        return pdr

    loss = to_float(row.get("loss_pct"))
    return None if loss is None else 100.0 - loss


REFERENCE_PAYLOAD_BYTES = 16.0


def is_baseline(row):
    return (row.get("cenario") or "").strip().lower() == "baseline"


def is_no_obstacle(row):
    value = (row.get("obstaculo") or "").strip().lower()
    return value in ("", "nenhum", "sem", "none", "n/a", "na")


def session_key(row):
    """Identifica um boot/sessao sem contar uma vez por payload.

    CSVs novos trazem a coluna sessao. Nos CSVs antigos, cai para uma chave
    conservadora baseada no arquivo e nas metricas de sessao.
    """
    sessao = (row.get("sessao") or "").strip()
    if sessao:
        return (row.get("_arquivo", ""), sessao)
    return (
        row.get("_arquivo", ""), row.get("tecnologia", ""),
        row.get("link_setup_ms", ""), row.get("custo_radio_bytes", ""),
        row.get("custo_link_bytes", ""), row.get("min_free_heap", ""),
    )


def condition_label(row):
    dist = to_float(row.get("distancia_m"))
    obstacle = (row.get("obstaculo") or "nenhum").strip() or "nenhum"
    if dist is None:
        return obstacle
    if float(dist).is_integer():
        dist_text = str(int(dist))
    else:
        dist_text = str(dist)
    return "{} m | {}".format(dist_text, obstacle)


def load_rows(resultados_dir):
    rows = []
    csv_paths = sorted(glob.glob(os.path.join(resultados_dir, "*.csv")))

    for path in csv_paths:
        if os.path.basename(path) == "template.csv":
            continue

        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                # Linhas em branco no fim do arquivo, ou linhas sem
                # tecnologia preenchida (erro de transcricao manual).
                if not row.get("tecnologia"):
                    continue
                row["_arquivo"] = os.path.basename(path)
                rows.append(row)

    return rows


def summarize(rows):
    # chave: (tecnologia, cenario) -> lista de linhas
    groups = defaultdict(list)
    for row in rows:
        groups[(row["tecnologia"], row.get("cenario", ""))].append(row)

    print()
    print("=" * 88)
    print("RESUMO POR TECNOLOGIA E CENARIO - TODOS OS PAYLOADS")
    print("=" * 88)
    print("Curva individual de cada tecnologia. NAO use estes agregados para")
    print("comparar tecnologias entre si: cada uma testa uma lista diferente de")
    print("payloads (o ESP-NOW nao envia 512 nem 1200 bytes, por exemplo).")
    print("Para comparar, veja a tabela do nucleo comum mais abaixo.")
    print("-" * 88)
    header = "{:<10} {:<20} {:>5} {:>8} {:>8} {:>9} {:>9} {:>9} {:>11}".format(
        "tech", "cenario", "n", "loss_%", "pdr_%", "rtt_avg", "rtt_p95", "rssi_dbm", "throughput"
    )
    print(header)
    print("-" * len(header))

    for (tech, cenario), group_rows in sorted(groups.items()):
        losses = [to_float(r.get("loss_pct")) for r in group_rows if to_float(r.get("loss_pct")) is not None]
        rtt_avgs = [to_float(r.get("rtt_avg_ms")) for r in group_rows if to_float(r.get("rtt_avg_ms")) is not None]
        rtt_p95s = [to_float(r.get("rtt_p95_ms")) for r in group_rows if to_float(r.get("rtt_p95_ms")) is not None]
        throughputs = [to_float(r.get("throughput_kbps")) for r in group_rows if to_float(r.get("throughput_kbps")) is not None]
        pdrs = [to_float(r.get("pdr_pct")) for r in group_rows if to_float(r.get("pdr_pct")) is not None]
        rssis = [to_float(r.get("rssi_avg_dbm")) for r in group_rows if to_float(r.get("rssi_avg_dbm")) is not None]

        # PDR e o complemento da perda; se a coluna nao existir (CSV
        # de uma versao anterior do firmware), da para deduzi-la.
        if not pdrs and losses:
            pdrs = [100.0 - loss for loss in losses]

        print("{:<10} {:<20} {:>5} {:>8} {:>8} {:>9} {:>9} {:>9} {:>11}".format(
            tech,
            cenario or "(sem cenario)",
            len(group_rows),
            "{:.1f}".format(statistics.mean(losses)) if losses else "-",
            "{:.1f}".format(statistics.mean(pdrs)) if pdrs else "-",
            "{:.2f}".format(statistics.mean(rtt_avgs)) if rtt_avgs else "-",
            "{:.2f}".format(statistics.mean(rtt_p95s)) if rtt_p95s else "-",
            "{:.1f}".format(statistics.mean(rssis)) if rssis else "-",
            "{:.1f}".format(statistics.mean(throughputs)) if throughputs else "NA",
        ))

    print()

    # Tempo de estabelecimento do link: um valor por sessao/boot.
    # Nao usamos set(valor), pois duas sessoes reais podem ter exatamente o
    # mesmo tempo e ainda assim devem contar como duas repeticoes.
    link_setup_by_tech = defaultdict(list)
    seen_sessions = set()
    for row in rows:
        ms = to_float(row.get("link_setup_ms"))
        if ms is None:
            continue
        key = (row["tecnologia"], session_key(row))
        if key in seen_sessions:
            continue
        seen_sessions.add(key)
        link_setup_by_tech[row["tecnologia"]].append(ms)

    if link_setup_by_tech:
        print("Tempo de estabelecimento do link (link_setup_ms), por sessao observada:")
        for tech, values in sorted(link_setup_by_tech.items()):
            values_sorted = sorted(values)
            print("  {:<10} {} ms".format(tech, ", ".join("{:.0f}".format(v) for v in values_sorted)))
        print()

    summarize_nucleo(rows)
    summarize_memory(rows)


def summarize_nucleo(rows):
    """Comparativo restrito aos payloads presentes em todas as tecnologias."""
    nucleo = common_payloads(rows)

    if not nucleo:
        # Silencio aqui seria pior que um aviso: quem le o resumo precisa
        # saber que a comparacao entre tecnologias nao pode ser feita, e
        # por que - tipicamente porque uma delas foi coletada com uma lista
        # de payloads que nao encosta nas outras.
        by_tech = defaultdict(set)
        for row in rows:
            payload = to_float(row.get("payload_bytes"))
            if payload is not None:
                by_tech[row["tecnologia"]].add(payload)

        print("=" * 88)
        print("COMPARATIVO ENTRE TECNOLOGIAS - INDISPONIVEL")
        print("=" * 88)
        print("Nao ha nenhum tamanho de payload presente em todas as tecnologias")
        print("coletadas, entao nao existe linha comparavel entre elas:")
        for tech, payloads in sorted(by_tech.items()):
            print("  {:<10} {}".format(
                tech, ", ".join(str(int(p)) for p in sorted(payloads))
            ))
        print()
        print("Recolete a tecnologia divergente com a lista de payloads atual")
        print("dos sketches para que a comparacao volte a ser possivel.")
        print()
        return

    comparaveis = [r for r in rows if to_float(r.get("payload_bytes")) in nucleo]

    if not comparaveis:
        return

    groups = defaultdict(list)
    for row in comparaveis:
        groups[(row["tecnologia"], row.get("cenario", ""))].append(row)

    print("=" * 88)
    print("COMPARATIVO ENTRE TECNOLOGIAS - NUCLEO COMUM ({} bytes)".format(
        ", ".join(str(int(p)) for p in sorted(nucleo))
    ))
    print("=" * 88)
    print("Estas sao as linhas comparaveis: mesmo payload de aplicacao em")
    print("todas as tecnologias presentes nos dados.")
    print("-" * 88)

    header = "{:<10} {:<20} {:>5} {:>8} {:>9} {:>9} {:>9} {:>11}".format(
        "tech", "cenario", "n", "pdr_%", "rtt_avg", "rtt_p50", "rssi_dbm", "throughput"
    )
    print(header)
    print("-" * len(header))

    for (tech, cenario), group_rows in sorted(groups.items()):
        def media(campo):
            vals = [to_float(r.get(campo)) for r in group_rows]
            vals = [v for v in vals if v is not None]
            return statistics.mean(vals) if vals else None

        pdrs = [pdr_of(r) for r in group_rows]
        pdrs = [v for v in pdrs if v is not None]

        rtt_avg, rtt_p50 = media("rtt_avg_ms"), media("rtt_p50_ms")
        rssi, thr = media("rssi_avg_dbm"), media("throughput_kbps")

        print("{:<10} {:<20} {:>5} {:>8} {:>9} {:>9} {:>9} {:>11}".format(
            tech,
            cenario or "(sem cenario)",
            len(group_rows),
            "{:.1f}".format(statistics.mean(pdrs)) if pdrs else "-",
            "{:.2f}".format(rtt_avg) if rtt_avg is not None else "-",
            "{:.2f}".format(rtt_p50) if rtt_p50 is not None else "-",
            "{:.1f}".format(rssi) if rssi is not None else "-",
            "{:.1f}".format(thr) if thr is not None else "NA",
        ))

    print()


def memory_by_tech(rows):
    """Custo de RAM por tecnologia: uma medicao por sessao/boot.

    A coluna sessao evita contar a mesma medicao uma vez por payload e, ao
    mesmo tempo, preserva repeticoes reais mesmo quando duas sessoes produzem
    exatamente os mesmos numeros de heap.
    """
    by_tech = defaultdict(list)
    seen_sessions = set()

    for row in rows:
        radio = to_float(row.get("custo_radio_bytes"))
        link = to_float(row.get("custo_link_bytes"))
        min_free = to_float(row.get("min_free_heap"))

        if radio is None and link is None:
            continue

        key = (row["tecnologia"], session_key(row))
        if key in seen_sessions:
            continue
        seen_sessions.add(key)
        by_tech[row["tecnologia"]].append((radio or 0.0, link or 0.0, min_free))

    return by_tech


def summarize_memory(rows):
    by_tech = memory_by_tech(rows)

    if not by_tech:
        return

    print("Custo incremental de heap (antes do radio x apos enlace pronto):")
    header = "  {:<10} {:>14} {:>14} {:>14} {:>14}".format(
        "tech", "radio (B)", "enlace (B)", "total (B)", "min_free (B)"
    )
    print(header)
    print("  " + "-" * (len(header) - 2))

    for tech, measurements in sorted(by_tech.items()):
        radios = [m[0] for m in measurements]
        links = [m[1] for m in measurements]
        min_frees = [m[2] for m in measurements if m[2] is not None]

        radio_avg = statistics.mean(radios)
        link_avg = statistics.mean(links)

        print("  {:<10} {:>14,.0f} {:>14,.0f} {:>14,.0f} {:>14}".format(
            tech,
            radio_avg,
            link_avg,
            radio_avg + link_avg,
            "{:,.0f}".format(statistics.mean(min_frees)) if min_frees else "-",
        ))

    print()


def group_for_plot(rows, key_field, tech_field="tecnologia"):
    """Agrupa por (tecnologia, valor de key_field), devolvendo medias."""
    buckets = defaultdict(list)
    for row in rows:
        key_value = to_float(row.get(key_field))
        if key_value is None:
            continue
        buckets[(row[tech_field], key_value)].append(row)
    return buckets


def make_plots(rows, out_dir):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib nao encontrado - pulando geracao de graficos.")
        print("Instale com: pip install matplotlib")
        return

    os.makedirs(out_dir, exist_ok=True)

    techs = sorted(set(row["tecnologia"] for row in rows))
    colors = {}
    palette = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]
    for i, tech in enumerate(techs):
        colors[tech] = palette[i % len(palette)]

    # Um grafico pulado nao pode deixar para tras o PNG de uma rodada
    # anterior - quem abre a pasta acharia que ele reflete os dados atuais.
    def remove_stale(filename):
        path = os.path.join(out_dir, filename)
        if os.path.exists(path):
            os.remove(path)
            print("Removido (desatualizado): " + filename)

    # rtt_16bytes.png foi incorporado ao comparativo_baseline.png
    remove_stale("rtt_16bytes.png")

    # -------- RTT medio vs distancia (cenarios de alcance) --------
    dist_rows = [
        r for r in rows
        if to_float(r.get("distancia_m")) is not None
        and to_float(r.get("payload_bytes")) == REFERENCE_PAYLOAD_BYTES
        and is_no_obstacle(r)
    ]
    # Com uma distancia so, "vs. distancia" vira um ponto isolado num eixo
    # de 0.96 a 1.04 m - nao diz nada, entao espera haver ao menos duas.
    distances = set(to_float(r.get("distancia_m")) for r in dist_rows)
    if len(distances) >= 2:
        buckets = group_for_plot(dist_rows, "distancia_m")
        fig, ax = plt.subplots(figsize=(7, 5))
        for tech in techs:
            points = sorted(
                (dist, group) for (t, dist), group in buckets.items() if t == tech
            )
            if not points:
                continue
            xs = [p[0] for p in points]
            avgs = [statistics.mean(to_float(r.get("rtt_avg_ms"), 0) or 0 for r in p[1]) for p in points]
            p95s = [statistics.mean(to_float(r.get("rtt_p95_ms"), 0) or 0 for r in p[1]) for p in points]
            errs = [max(0, p95 - avg) for avg, p95 in zip(avgs, p95s)]
            # Assimetrica: so para cima. O p95 nunca fica abaixo da media,
            # e um yerr simples desenharia a barra para os dois lados.
            ax.errorbar(
                xs, avgs, yerr=[[0] * len(errs), errs],
                label=tech, marker="o", color=colors[tech], capsize=4,
            )

        ax.set_xlabel("Distancia (m)")
        ax.set_ylabel("RTT medio (ms), barra = p95")
        ax.set_title("RTT vs. distancia - payload 16 B, sem obstaculo")
        ax.legend()
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "rtt_vs_distancia.png"), dpi=150)
        plt.close(fig)
        print("Gerado: rtt_vs_distancia.png")

        # -------- perda vs distancia --------
        fig, ax = plt.subplots(figsize=(7, 5))
        for tech in techs:
            points = sorted(
                (dist, group) for (t, dist), group in buckets.items() if t == tech
            )
            if not points:
                continue
            xs = [p[0] for p in points]
            losses = [statistics.mean(to_float(r.get("loss_pct"), 0) or 0 for r in p[1]) for p in points]
            ax.plot(xs, losses, label=tech, marker="o", color=colors[tech])

        ax.set_xlabel("Distancia (m)")
        ax.set_ylabel("Perda de pacotes (%)")
        ax.set_title("Perda vs. distancia - payload 16 B, sem obstaculo")
        ax.axhline(20, color="gray", linestyle="--", linewidth=1, label="limiar 20%")
        ax.legend()
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "perda_vs_distancia.png"), dpi=150)
        plt.close(fig)
        print("Gerado: perda_vs_distancia.png")
    else:
        print("Menos de duas distancias com 16 B sem obstaculo - pulando graficos de alcance.")
        remove_stale("rtt_vs_distancia.png")
        remove_stale("perda_vs_distancia.png")

    # -------- throughput vs payload (exclui Zigbee, que e NA) --------
    # O Zigbee entrava excluido daqui quando o teste dele usava o cluster
    # On/Off (1 bit, throughput "NA"). Com o cluster de ping proprio ele tem
    # throughput de verdade nos payloads de 16 e 32 bytes, entao participa
    # como as outras - a curva dele so e mais curta, porque o quadro do
    # 802.15.4 nao comporta 100 nem 200 bytes.
    payload_rows = [
        r for r in rows
        if is_baseline(r) and to_float(r.get("throughput_kbps")) is not None
    ]
    if payload_rows:
        buckets = group_for_plot(payload_rows, "payload_bytes")
        fig, ax = plt.subplots(figsize=(7, 5))
        for tech in techs:
            points = sorted(
                (pl, group) for (t, pl), group in buckets.items() if t == tech
            )
            if not points:
                continue
            xs = [p[0] for p in points]
            ys = [statistics.mean(to_float(r.get("throughput_kbps"), 0) or 0 for r in p[1]) for p in points]
            ax.plot(xs, ys, label=tech, marker="o", color=colors[tech])

        ax.set_xlabel("Payload (bytes)")
        ax.set_ylabel("Goodput do ping-pong (kbps, escala log)")
        ax.set_title("Goodput do ping-pong vs. payload - baseline")
        # Linear, o Wi-Fi em 1200 B achata BLE e Zigbee contra o zero.
        ax.set_yscale("log")
        ax.legend()
        ax.grid(True, which="both", alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "throughput_vs_payload.png"), dpi=150)
        plt.close(fig)
        print("Gerado: throughput_vs_payload.png")
    else:
        print("Nenhuma linha com throughput_kbps utilizavel - pulando grafico de payload.")

    # -------- comparativo no baseline --------
    # Restrito ao nucleo comum pelo mesmo motivo da tabela: incluir os
    # payloads extras do Wi-Fi inflaria a barra dele contra tecnologias que
    # nem conseguem enviar aqueles tamanhos.
    nucleo = common_payloads(rows)
    baseline_rows = [
        r for r in rows
        if (r.get("cenario") or "").strip().lower() == "baseline"
        and to_float(r.get("payload_bytes")) in nucleo
    ]
    #
    # Uma barra por (tecnologia, payload) em vez da media de todos os
    # payloads juntos: misturar 16 e 32 B numa barra so esconderia quanto o
    # RTT cresce com o payload. Alem da media, marca p50 e p95 - a media
    # sozinha esconde, por exemplo, que o BLE fica preso em degraus do
    # connection interval em vez de variar em torno dela.
    if baseline_rows:
        buckets = defaultdict(lambda: {"avg": [], "p50": [], "p95": []})
        for r in baseline_rows:
            key = (r["tecnologia"], to_float(r.get("payload_bytes")))
            for field, col in (("avg", "rtt_avg_ms"), ("p50", "rtt_p50_ms"), ("p95", "rtt_p95_ms")):
                value = to_float(r.get(col))
                if value is not None:
                    buckets[key][field].append(value)

        names = sorted(set(t for t, _ in buckets))
        payloads = sorted(nucleo)
        if names:
            fig, ax = plt.subplots(figsize=(7, 5))
            width = 0.8 / max(1, len(payloads))
            hatches = ["", "//", "..", "xx"]
            p50_labeled = p95_labeled = False
            for idx, payload in enumerate(payloads):
                for x, tech in enumerate(names):
                    stats = buckets.get((tech, payload))
                    if not stats or not stats["avg"]:
                        continue
                    xpos = x - 0.4 + width / 2 + idx * width
                    avg = statistics.mean(stats["avg"])
                    ax.bar(
                        xpos, avg, width=width, color=colors[tech],
                        hatch=hatches[idx % len(hatches)], edgecolor="white",
                    )
                    if stats["p95"]:
                        p95 = statistics.mean(stats["p95"])
                        ax.errorbar(
                            xpos, avg, yerr=[[0], [max(0, p95 - avg)]],
                            color="black", capsize=3, linewidth=1,
                            label=None if p95_labeled else "p95",
                        )
                        p95_labeled = True
                    if stats["p50"]:
                        ax.plot(
                            xpos, statistics.mean(stats["p50"]), marker="_",
                            markersize=14, markeredgewidth=2, color="black",
                            linestyle="none",
                            label=None if p50_labeled else "p50 (mediana)",
                        )
                        p50_labeled = True

            # Legenda dos payloads pelo padrao de preenchimento
            for idx, payload in enumerate(payloads):
                ax.bar(
                    0, 0, color="gray", hatch=hatches[idx % len(hatches)],
                    edgecolor="white", label="{} B".format(int(payload)),
                )
            ax.set_xticks(range(len(names)))
            ax.set_xticklabels(names)
            ax.set_ylabel("RTT no baseline (ms) - barra = media")
            ax.set_title(
                "RTT no baseline - payloads do nucleo comum\n"
                "({} bytes: os presentes em todas as tecnologias)".format(
                    ", ".join(str(int(p)) for p in payloads)
                )
            )
            ax.legend()
            ax.grid(True, axis="y", alpha=0.3)
            fig.tight_layout()
            fig.savefig(os.path.join(out_dir, "comparativo_baseline.png"), dpi=150)
            plt.close(fig)
            print("Gerado: comparativo_baseline.png")
    else:
        print("Nenhuma linha com cenario 'baseline' - pulando comparativo.")

    # -------- PDR vs RSSI (curva PDR x RSSI) --------
    rssi_rows = [
        r for r in rows
        if to_float(r.get("payload_bytes")) == REFERENCE_PAYLOAD_BYTES
        and to_float(r.get("rssi_avg_dbm")) is not None and pdr_of(r) is not None
    ]
    if rssi_rows:
        fig, ax = plt.subplots(figsize=(7, 5))
        for tech in techs:
            points = sorted(
                (to_float(r.get("rssi_avg_dbm")), pdr_of(r))
                for r in rssi_rows if r["tecnologia"] == tech
            )
            if not points:
                continue
            ax.scatter(
                [p[0] for p in points], [p[1] for p in points],
                label=tech, color=colors[tech], alpha=0.7, s=40,
            )

        ax.set_xlabel("RSSI medio no lote (dBm)")
        ax.set_ylabel("Packet Delivery Ratio (%)")
        ax.set_title("Qualidade do enlace: PDR vs. RSSI - payload 16 B")
        ax.set_ylim(-5, 105)
        ax.axhline(80, color="gray", linestyle="--", linewidth=1, label="PDR 80%")
        ax.legend()
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "pdr_vs_rssi.png"), dpi=150)
        plt.close(fig)
        print("Gerado: pdr_vs_rssi.png")
    else:
        print("Nenhuma linha de 16 B com RSSI - pulando grafico PDR vs RSSI.")

    # -------- PDR por condicao fisica, payload fixo de 16 bytes --------
    condition_rows = [
        r for r in rows
        if to_float(r.get("payload_bytes")) == REFERENCE_PAYLOAD_BYTES
        and pdr_of(r) is not None
        and (to_float(r.get("distancia_m")) is not None or (r.get("obstaculo") or "").strip())
    ]
    if condition_rows:
        buckets = defaultdict(lambda: defaultdict(list))
        for r in condition_rows:
            buckets[condition_label(r)][r["tecnologia"]].append(pdr_of(r))

        conditions = sorted(buckets.keys())
        present_techs = [t for t in techs if any(t in buckets[c] for c in conditions)]
        # Uma condicao so e o baseline repetido - comparar "por condicao"
        # exige ao menos duas.
        if len(conditions) >= 2 and present_techs:
            fig, ax = plt.subplots(figsize=(max(7, len(conditions) * 1.2), 5))
            width = 0.8 / max(1, len(present_techs))
            base = list(range(len(conditions)))
            for idx, tech in enumerate(present_techs):
                xs = [x - 0.4 + width / 2 + idx * width for x in base]
                ys = [
                    statistics.mean(buckets[c][tech]) if buckets[c].get(tech) else float("nan")
                    for c in conditions
                ]
                ax.bar(xs, ys, width=width, label=tech, color=colors[tech])
            ax.set_xticks(base)
            ax.set_xticklabels(conditions, rotation=25, ha="right")
            ax.set_ylabel("Packet Delivery Ratio (%)")
            ax.set_title("PDR por distancia e obstaculo - payload 16 B")
            ax.set_ylim(0, 105)
            ax.legend()
            ax.grid(True, axis="y", alpha=0.3)
            fig.tight_layout()
            fig.savefig(os.path.join(out_dir, "pdr_por_condicao_16bytes.png"), dpi=150)
            plt.close(fig)
            print("Gerado: pdr_por_condicao_16bytes.png")
        else:
            print("Menos de duas condicoes fisicas - pulando grafico de PDR por condicao.")
            remove_stale("pdr_por_condicao_16bytes.png")

    # -------- variabilidade de RTT (jitter operacional), baseline 16 B --------
    jitter_rows = [
        r for r in rows
        if is_baseline(r)
        and to_float(r.get("payload_bytes")) == REFERENCE_PAYLOAD_BYTES
        and to_float(r.get("rtt_p50_ms")) is not None
        and to_float(r.get("rtt_p95_ms")) is not None
    ]
    # p95 - p50 em vez do desvio-padrao: com 100 amostras, um ou dois picos
    # isolados (ex.: a estacao Wi-Fi acordando do modem sleep) dominam o
    # desvio-padrao e fazem o enlace parecer instavel o tempo todo. O
    # espalhamento entre mediana e p95 mede a variacao tipica, e os picos
    # raros continuam visiveis no rtt_p99/rtt_max do resumo.
    if jitter_rows:
        by_tech = defaultdict(list)
        for r in jitter_rows:
            spread = to_float(r.get("rtt_p95_ms")) - to_float(r.get("rtt_p50_ms"))
            by_tech[r["tecnologia"]].append(max(0.0, spread))
        names = sorted(by_tech.keys())
        if names:
            fig, ax = plt.subplots(figsize=(6, 5))
            means = [statistics.mean(by_tech[t]) for t in names]
            ax.bar(names, means, color=[colors[t] for t in names])
            ax.set_ylabel("p95 - p50 do RTT (ms)")
            ax.set_title("Variabilidade do RTT - baseline, payload 16 B")
            ax.grid(True, axis="y", alpha=0.3)
            fig.tight_layout()
            fig.savefig(os.path.join(out_dir, "jitter_16bytes_baseline.png"), dpi=150)
            plt.close(fig)
            print("Gerado: jitter_16bytes_baseline.png")

    # -------- tempo de estabelecimento do link por sessao --------
    setup_values = defaultdict(list)
    seen_setup_sessions = set()
    for r in rows:
        ms = to_float(r.get("link_setup_ms"))
        if ms is None:
            continue
        key = (r["tecnologia"], session_key(r))
        if key in seen_setup_sessions:
            continue
        seen_setup_sessions.add(key)
        setup_values[r["tecnologia"]].append(ms)

    if setup_values:
        names = sorted(setup_values.keys())
        means = [statistics.mean(setup_values[t]) for t in names]
        fig, ax = plt.subplots(figsize=(6, 5))
        ax.bar(names, means, color=[colors[t] for t in names])
        ax.set_ylabel("Tempo ate enlace pronto (ms)")
        ax.set_title("Tempo de estabelecimento do link")
        ax.grid(True, axis="y", alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "link_setup_por_tecnologia.png"), dpi=150)
        plt.close(fig)
        print("Gerado: link_setup_por_tecnologia.png")

    # -------- custo de memoria por tecnologia --------
    mem_by_tech = memory_by_tech(rows)
    if mem_by_tech:
        fig, ax = plt.subplots(figsize=(6, 5))
        names = sorted(mem_by_tech.keys())
        radios = [statistics.mean([m[0] for m in mem_by_tech[t]]) / 1024.0 for t in names]
        links = [statistics.mean([m[1] for m in mem_by_tech[t]]) / 1024.0 for t in names]

        ax.bar(names, radios, color=[colors[t] for t in names])
        ax.bar(
            names, links, bottom=radios, color="white",
            edgecolor=[colors[t] for t in names], hatch="//",
        )

        # A cor de cada barra identifica a tecnologia, nao a camada -
        # entao a legenda usa amostras neutras, so para explicar a
        # diferenca entre a parte solida e a hachurada.
        from matplotlib.patches import Patch
        ax.legend(handles=[
            Patch(facecolor="gray", label="pilha de radio"),
            Patch(facecolor="white", edgecolor="gray", hatch="//", label="enlace com a estacao B"),
        ])

        ax.set_ylabel("RAM consumida (KiB de heap)")
        ax.set_title("Custo de memoria por tecnologia")
        ax.grid(True, axis="y", alpha=0.3)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "memoria_por_tecnologia.png"), dpi=150)
        plt.close(fig)
        print("Gerado: memoria_por_tecnologia.png")
    else:
        print("Nenhuma linha com custo_radio_bytes preenchido - pulando grafico de memoria.")


def main():
    args = parse_args()
    resultados_dir = os.path.abspath(args.resultados_dir)

    if not os.path.isdir(resultados_dir):
        sys.stderr.write("Pasta de resultados nao encontrada: {}\n".format(resultados_dir))
        sys.exit(1)

    rows = load_rows(resultados_dir)

    if not rows:
        print("Nenhuma linha de resultado encontrada em {} (apenas template.csv?).".format(resultados_dir))
        print("Rode os testes e copie/gere os CSVs antes de analisar - ver readme.md.")
        return

    print("Carregadas {} linhas de resultado de {} arquivo(s) CSV em {}".format(
        len(rows),
        len(set(r["_arquivo"] for r in rows)),
        resultados_dir,
    ))

    summarize(rows)

    if not args.sem_graficos:
        out_dir = os.path.join(resultados_dir, "graficos")
        make_plots(rows, out_dir)
        print()
        print("Graficos salvos em: {}".format(out_dir))


if __name__ == "__main__":
    main()
