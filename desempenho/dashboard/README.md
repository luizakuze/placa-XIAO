# Dashboard de Análise de Desempenho

Aplicação web interativa para visualizar gráficos de desempenho de tecnologias sem fio.

Permite upload de arquivo CSV com dados de testes e renderiza 4 gráficos interativos usando Plotly.

## Instalação

Pré-requisitos: Python 3.7+

```bash
cd desempenho/dashboard
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

## Uso

Iniciar o servidor:

```bash
source venv/bin/activate
python3 run.py
```

A aplicação abre automaticamente em http://localhost:5000

Arraste seu arquivo CSV na página ou clique para selecionar. O CSV deve conter as colunas:
- tecnologia (BLE, ESPNOW, WIFI, ZIGBEE)
- payload_bytes
- cenario (baseline, distancia, etc)
- rtt_avg_ms, pdr_pct, rssi_avg_dbm, link_setup_ms, throughput_kbps

## Exemplo de Tela

Após upload do CSV, a dashboard exibe 4 gráficos interativos:

1. Goodput vs. Payload - Taxa de dados por tamanho de payload (escala logarítmica)
2. PDR vs. RSSI - Entrega de pacotes em função do sinal recebido
3. RTT no Baseline - Latência mediana para cada tecnologia
4. Link Setup Time - Tempo para estabelecer conexão

Todos os gráficos são interativos: zoom, pan, hover para ver valores, download como PNG.

## Arquivos

- app.py - Servidor Flask com processamento de CSV e geração de gráficos
- templates/index.html - Interface web com drag-and-drop
- requirements.txt - Dependências Python (Flask, Plotly)
- run.py - Script que inicia o servidor e abre o navegador

## Parar a Aplicação

Pressione Ctrl+C no terminal onde o servidor está rodando.
