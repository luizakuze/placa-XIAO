# Resultados de Desempenho

Dados coletados em 23 de setembro de 2026. Arquivo principal: `2026-09-23.csv`

Para análise detalhada com explicações de cada métrica e recomendações, consulte [ANALISE.md](ANALISE.md).

## Gráficos Gerados

Os gráficos foram gerados automaticamente pelo script `ferramentas/analisar_resultados.py`.

### throughput_vs_payload.png

Taxa de dados úteis transmitidos (kbps) em função do tamanho do payload em bytes. Mostra como cada tecnologia escala com payloads maiores.

### comparativo_baseline.png

Latência mediana (RTT) em milissegundos no cenário baseline (1 metro, sem obstáculo) para cada tecnologia. Núcleo comum com 16 e 32 bytes.

### pdr_vs_rssi.png

Relação entre força do sinal recebido (RSSI em dBm) e taxa de entrega de pacotes (PDR em %) para payload de 16 bytes.

### jitter_16bytes_baseline.png

Desvio padrão do RTT em milissegundos para o payload de referência (16 bytes) no cenário baseline.

### link_setup_por_tecnologia.png

Tempo necessário para estabelecer conexão entre as duas placas em milissegundos para cada tecnologia.

### memoria_por_tecnologia.png

Custo de memória em bytes usado por cada tecnologia, dividido entre inicialização do rádio e estabelecimento do enlace.

## Para Regenerar

Execute o script de análise com os dados do CSV:

```bash
python3 ferramentas/analisar_resultados.py
```

Para gerar apenas o resumo sem gráficos:

```bash
python3 ferramentas/analisar_resultados.py --sem-graficos
```

## Estrutura

```
resultados/
  2026-09-23.csv         Dados brutos coletados
  template.csv           Modelo para entrada manual
  graficos/              Gráficos gerados
  descartados/           Resultados descartados
```
