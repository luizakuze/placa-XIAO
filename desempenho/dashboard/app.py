#!/usr/bin/env python3
"""Dashboard simples para análise de desempenho"""

from flask import Flask, render_template, request, jsonify
import plotly.graph_objects as go
import csv
from collections import defaultdict
import statistics

app = Flask(__name__)

def to_float(value, default=None):
    try:
        if value is None or value == "" or str(value).strip().upper() == "NA":
            return default
        return float(value)
    except:
        return default

def parse_csv(content):
    """Parse CSV simples"""
    rows = []
    lines = content.strip().split('\n')
    if len(lines) < 2:
        return rows

    header = lines[0].split(',')
    for line in lines[1:]:
        if not line.strip():
            continue
        values = line.split(',')
        row = {}
        for i, h in enumerate(header):
            h = h.strip()
            row[h] = values[i].strip() if i < len(values) else ""
        if row.get("tecnologia"):
            rows.append(row)
    return rows

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/api/upload', methods=['POST'])
def upload():
    try:
        if 'file' not in request.files:
            return jsonify({'error': 'Nenhum arquivo'}), 400

        file = request.files['file']
        if not file or file.filename == '':
            return jsonify({'error': 'Arquivo vazio'}), 400

        content = file.read().decode('utf-8')
        rows = parse_csv(content)

        if not rows:
            return jsonify({'error': 'Nenhuma linha válida'}), 400

        print(f"DEBUG: Carregadas {len(rows)} linhas")
        print(f"DEBUG: Primeira linha: {rows[0]}")

        graphs = {}
        colors = {"BLE": "#1f77b4", "ESPNOW": "#ff7f0e", "WIFI": "#2ca02c", "ZIGBEE": "#d62728"}

        # 1. Goodput vs Payload
        payload_data = defaultdict(lambda: defaultdict(list))
        for r in rows:
            if r.get("cenario", "").strip().lower() == "baseline":
                tech = r.get("tecnologia", "").strip()
                payload = to_float(r.get("payload_bytes"))
                throughput = to_float(r.get("throughput_kbps"))
                if tech and payload is not None and throughput is not None:
                    payload_data[tech][payload].append(throughput)

        if payload_data:
            fig = go.Figure()
            for tech in sorted(payload_data.keys()):
                payloads = sorted(payload_data[tech].keys())
                means = [statistics.mean(payload_data[tech][p]) for p in payloads]
                fig.add_trace(go.Scatter(
                    x=payloads, y=means, mode='markers+lines', name=tech,
                    marker=dict(size=10, color=colors.get(tech, "#999")),
                    line=dict(color=colors.get(tech, "#999"), width=2)
                ))
            fig.update_layout(
                title="Goodput vs. Payload",
                xaxis_title="Payload (bytes)",
                yaxis_title="Goodput (kbps)",
                height=500,
                template='plotly_white',
                hovermode='closest',
                xaxis=dict(showgrid=True),
                yaxis=dict(type="log", showgrid=True, gridwidth=1, gridcolor='lightgray')
            )
            graphs['goodput'] = fig.to_json()
            print("Goodput gerado")

        # 2. PDR vs RSSI
        rssi_data = defaultdict(list)
        for r in rows:
            if to_float(r.get("payload_bytes")) == 16:
                tech = r.get("tecnologia", "").strip()
                rssi = to_float(r.get("rssi_avg_dbm"))
                pdr = to_float(r.get("pdr_pct"))
                if not pdr:
                    loss = to_float(r.get("loss_pct"))
                    pdr = 100 - loss if loss is not None else None

                if tech and rssi is not None and pdr is not None:
                    rssi_data[tech].append((rssi, pdr))

        if rssi_data:
            fig = go.Figure()
            for tech in sorted(rssi_data.keys()):
                if rssi_data[tech]:
                    xs = [p[0] for p in rssi_data[tech]]
                    ys = [p[1] for p in rssi_data[tech]]
                    fig.add_trace(go.Scatter(
                        x=xs, y=ys, mode='markers', name=tech,
                        marker=dict(size=12, color=colors.get(tech, "#999"))
                    ))
            fig.update_layout(
                title="PDR vs. RSSI (16B)",
                xaxis_title="RSSI (dBm)",
                yaxis_title="PDR (%)",
                height=500,
                template='plotly_white',
                yaxis=dict(range=[-5, 105], showgrid=True, gridwidth=1, gridcolor='lightgray'),
                xaxis=dict(showgrid=True, gridwidth=1, gridcolor='lightgray'),
                hovermode='closest'
            )
            graphs['pdr_rssi'] = fig.to_json()
            print("PDR/RSSI gerado")

        # 3. RTT Baseline
        rtt_data = defaultdict(list)
        for r in rows:
            if r.get("cenario", "").strip().lower() == "baseline":
                tech = r.get("tecnologia", "").strip()
                rtt = to_float(r.get("rtt_avg_ms"))
                if tech and rtt is not None:
                    rtt_data[tech].append(rtt)

        if rtt_data:
            fig = go.Figure()
            techs = sorted(rtt_data.keys())
            means = [statistics.mean(rtt_data[t]) for t in techs]
            fig.add_trace(go.Bar(
                x=techs, y=means,
                marker=dict(color=[colors.get(t, "#999") for t in techs])
            ))
            fig.update_layout(
                title="RTT no Baseline",
                yaxis_title="RTT (ms)",
                height=500,
                showlegend=False,
                template='plotly_white',
                yaxis=dict(showgrid=True, gridwidth=1, gridcolor='lightgray'),
                xaxis=dict(showgrid=False)
            )
            # Adicionar valores nas barras
            fig.update_traces(text=[f'{v:.2f}' for v in means], textposition='outside')
            graphs['rtt'] = fig.to_json()
            print("RTT gerado")

        # 4. Link Setup
        setup_data = defaultdict(list)
        for r in rows:
            tech = r.get("tecnologia", "").strip()
            setup = to_float(r.get("link_setup_ms"))
            if tech and setup is not None:
                setup_data[tech].append(setup)

        if setup_data:
            fig = go.Figure()
            techs = sorted(setup_data.keys())
            means = [statistics.mean(setup_data[t]) for t in techs]
            fig.add_trace(go.Bar(
                x=techs, y=means,
                marker=dict(color=[colors.get(t, "#999") for t in techs])
            ))
            fig.update_layout(
                title="Link Setup Time",
                yaxis_title="Tempo (ms)",
                height=500,
                showlegend=False,
                template='plotly_white',
                yaxis=dict(showgrid=True, gridwidth=1, gridcolor='lightgray'),
                xaxis=dict(showgrid=False)
            )
            # Adicionar valores nas barras
            fig.update_traces(text=[f'{v:.0f}ms' for v in means], textposition='outside')
            graphs['link_setup'] = fig.to_json()
            print("Link Setup gerado")

        print(f"Total de gráficos: {len(graphs)}")
        return jsonify({'graphs': graphs, 'success': True})

    except Exception as e:
        print(f"Erro: {e}")
        import traceback
        traceback.print_exc()
        return jsonify({'error': str(e)}), 500

if __name__ == '__main__':
    app.run(debug=True, port=5000)
