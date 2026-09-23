#!/usr/bin/env python3
"""
Captura automatica dos resultados do testbed de desempenho (Wi-Fi vs
ESP-NOW vs BLE vs Zigbee) direto da porta serial da estacao A, sem
copiar/colar manualmente as linhas RESULT,... para a planilha.

Por que isto existe
--------------------
O firmware de cada estacao_a.ino imprime uma linha "RESULT,..." por
lote de teste (uma por tamanho de payload) e, uma unica vez, logo
depois que o link com a estacao B fica pronto, as linhas
"LINKSETUP,..." (tempo de estabelecimento do link) e "MEMORY,..."
(heap livre antes/depois de subir o radio).
Fazer essa transcricao na mao para o CSV funciona, mas e a parte do
testbed mais sujeita a erro humano (linha trocada, virgula a mais,
metadado do cenario esquecido). Este script conecta na porta serial,
interpreta essas tres linhas e ja grava a linha completa do CSV -
incluindo os metadados da execucao (data/hora/sessao/repeticao/cenario/distancia/obstaculo)
que so quem esta com as placas na mao sabe, e que por isso continuam
sendo passados por linha de comando ou prompt interativo.

Uso basico
----------
    python3 capturar_serial.py --port /dev/ttyACM0 --cenario baseline

Ou, sem passar todos os metadados de cenario na linha de comando, o
script pergunta interativamente pelo que faltar (Enter aceita o
padrao entre colchetes). Pressione Ctrl+C para encerrar a captura.

O terminal continua interativo: qualquer linha digitada e enviada
para a estacao A pela serial (por exemplo, digite "r" + Enter para
repetir o benchmark), exatamente como no Monitor Serial do Arduino.

Dependencia: pyserial (`pip install pyserial`).
"""

import argparse
import csv
import datetime
import glob
import os
import select
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write(
        "Erro: modulo 'pyserial' nao encontrado. Instale com:\n"
        "    pip install pyserial\n"
    )
    sys.exit(1)


# Colunas na mesma ordem de resultados/template.csv
CSV_COLUMNS = [
    "data", "hora", "sessao", "repeticao",
    "tecnologia", "cenario", "distancia_m", "obstaculo",
    "payload_bytes", "sent", "received", "loss_pct", "pdr_pct",
    "rtt_min_ms", "rtt_avg_ms", "rtt_max_ms", "rtt_stddev_ms",
    "throughput_kbps", "rtt_p50_ms", "rtt_p90_ms", "rtt_p95_ms",
    "rtt_p99_ms", "rssi_avg_dbm", "rssi_min_dbm", "rssi_amostras",
    "link_setup_ms", "heap_antes_radio", "heap_pos_radio",
    "heap_pos_link", "custo_radio_bytes", "custo_link_bytes",
    "min_free_heap", "observacoes",
]

# Campos da linha RESULT,... impressa pelo firmware, na ordem em que
# aparecem depois do prefixo "RESULT,". Ver comentarios no topo de
# cada desempenho/<tecnologia>/estacao_a.ino.
RESULT_FIELDS = [
    "tecnologia", "payload_bytes", "sent", "received", "loss_pct",
    "pdr_pct", "rtt_min_ms", "rtt_avg_ms", "rtt_max_ms",
    "rtt_stddev_ms", "throughput_kbps", "rtt_p50_ms", "rtt_p90_ms",
    "rtt_p95_ms", "rtt_p99_ms", "rssi_avg_dbm", "rssi_min_dbm",
    "rssi_amostras",
]

# Campos da linha MEMORY,... impressa uma vez por boot, depois do
# prefixo "MEMORY,". O primeiro campo e a tecnologia, que ja vem na
# linha RESULT - por isso nao vira coluna aqui.
MEMORY_FIELDS = [
    "heap_antes_radio", "heap_pos_radio", "heap_pos_link",
    "custo_radio_bytes", "custo_link_bytes", "min_free_heap",
]

# Valores por sessao (boot), capturados uma vez e repetidos em todas
# as linhas RESULT daquela sessao: o link so e estabelecido uma vez,
# e o custo de memoria do radio nao muda de um payload para outro.
SESSION_FIELDS = ["link_setup_ms"] + MEMORY_FIELDS


def parse_args():
    parser = argparse.ArgumentParser(
        description="Captura linhas RESULT/LINKSETUP/MEMORY da serial e grava no CSV de resultados.",
    )
    parser.add_argument(
        "--port", default="auto",
        help="Porta serial da estacao A (ex.: /dev/ttyACM0, COM5). "
             "O padrao, 'auto', descobre sozinho qual porta e a estacao A.",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (padrao: 115200)")
    parser.add_argument("--cenario", default=None, help="Nome do cenario (ex.: baseline, alcance-10m, obstaculo-2paredes)")
    parser.add_argument("--distancia", default=None, help="Distancia em metros entre as placas (deixe vazio se nao aplicavel)")
    parser.add_argument("--obstaculo", default=None, help="Descricao do obstaculo (ex.: '1 parede', 'nenhum')")
    parser.add_argument("--observacoes", default="", help="Observacoes livres para esta sessao")
    parser.add_argument("--data", default=None, help="Data do teste, AAAA-MM-DD (padrao: hoje)")
    parser.add_argument(
        "--out", default=None,
        help="Arquivo CSV de saida (padrao: resultados/<data>.csv, ao lado deste script)",
    )
    return parser.parse_args()


# Como as duas placas sao identicas e a numeracao das portas muda
# conforme a ordem em que voce pluga, o nome da porta nao diz qual
# estacao esta do outro lado. Quem diz e a mensagem de boot - por isso
# a deteccao automatica abre cada porta e le o que a placa se apresenta.
STATION_A_MARKERS = (
    "Station A ready",          # Wi-Fi e ESP-NOW
    "Scanning for Station B",   # BLE
    "Starting Station A",       # Zigbee
    "Waiting for Station B",    # Wi-Fi/Zigbee, se o boot ja tiver passado
    "LINKSETUP,", "MEMORY,", "RESULT,",
)

STATION_B_MARKERS = (
    "Station B ready",          # ESP-NOW e BLE
    "Connecting to Station A",  # Wi-Fi
    "Starting Station B",       # Zigbee
    "Looking for Station A",    # Zigbee
    "Waiting for Station A",    # ESP-NOW
    "HELLO -> Station A",       # Wi-Fi
)


def open_serial(port, baud):
    """Abre a porta sem acionar DTR/RTS (ver comentario em main)."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def detect_station_a_port(baud, seconds_per_port=8):
    """Descobre qual porta serial e a estacao A, pela mensagem de boot.

    Abrir a porta reinicia a placa (USB nativo do ESP32-C6), entao cada
    candidata se reapresenta e da para identificar quem e quem. A porta
    escolhida e fechada aqui e reaberta pelo fluxo normal - a placa
    reinicia mais uma vez, o que e justamente o que queremos para
    capturar o boot inteiro.
    """
    candidates = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))

    if not candidates:
        return None

    if len(candidates) == 1:
        return candidates[0]

    print("Procurando a estacao A entre: {}".format(", ".join(candidates)))

    for port in candidates:
        try:
            ser = open_serial(port, baud)
        except serial.SerialException as exc:
            print("  {}: nao deu para abrir ({})".format(port, exc))
            continue

        verdict = None
        deadline = time.monotonic() + seconds_per_port

        try:
            while time.monotonic() < deadline and verdict is None:
                line = ser.readline().decode("utf-8", errors="replace")
                if not line:
                    continue
                if any(marker in line for marker in STATION_A_MARKERS):
                    verdict = "A"
                elif any(marker in line for marker in STATION_B_MARKERS):
                    verdict = "B"
        finally:
            ser.close()

        print("  {}: {}".format(port, {
            "A": "estacao A", "B": "estacao B", None: "nao se identificou",
        }[verdict]))

        if verdict == "A":
            return port

    return None


def prompt_if_missing(value, label, default=""):
    if value is not None:
        return value
    reply = input("{} [{}]: ".format(label, default)).strip()
    return reply if reply else default


def ensure_csv_header(path):
    """Cria o cabecalho e migra CSVs antigos quando so faltam colunas novas.

    A migracao evita misturar linhas com quantidades diferentes de colunas no
    mesmo arquivo diario. Campos novos ficam vazios nas linhas historicas.
    """
    is_new = not os.path.exists(path) or os.path.getsize(path) == 0
    if is_new:
        with open(path, "a", newline="") as f:
            csv.writer(f).writerow(CSV_COLUMNS)
        return True

    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        old_header = reader.fieldnames or []
        old_rows = list(reader)

    if old_header == CSV_COLUMNS:
        return False

    unknown = [c for c in old_header if c not in CSV_COLUMNS]
    if unknown:
        raise RuntimeError(
            "CSV existente tem colunas desconhecidas e nao foi migrado: {}".format(
                ", ".join(unknown)
            )
        )

    tmp_path = path + ".migrando"
    with open(tmp_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
        writer.writeheader()
        for row in old_rows:
            writer.writerow({col: row.get(col, "") for col in CSV_COLUMNS})
    os.replace(tmp_path, path)
    print(">> CSV antigo migrado para incluir hora/sessao/repeticao.")
    return False


def append_result_row(path, metadata, result_fields, session_fields):
    row = dict(metadata)
    row.update(result_fields)

    for field in SESSION_FIELDS:
        row[field] = session_fields.get(field, "")

    with open(path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
        writer.writerow(row)


def parse_result_line(line):
    # Ex.: RESULT,WIFI,16,100,98,2.00,98.00,3.120,4.500,9.870,1.234,320.45,4.100,6.200,7.800,9.100,-42.3,-47,10
    parts = line.strip().split(",")
    if len(parts) < 1 + len(RESULT_FIELDS):
        return None  # firmware antigo ou linha truncada: ignora com segurança
    values = parts[1:1 + len(RESULT_FIELDS)]
    fields = dict(zip(RESULT_FIELDS, values))

    # Antes de cada lote o firmware reimprime o cabecalho da tabela,
    # que tambem comeca com "RESULT," - ele nao e um resultado.
    if fields["tecnologia"] == "tech":
        return None

    return fields


def parse_linksetup_line(line):
    # Ex.: LINKSETUP,WIFI,842
    parts = line.strip().split(",")
    if len(parts) != 3:
        return None, None
    return parts[1], parts[2]


def parse_memory_line(line):
    # Ex.: MEMORY,WIFI,289000,231000,229500,58000,1500,228100
    parts = line.strip().split(",")
    if len(parts) != 2 + len(MEMORY_FIELDS):
        return None, None
    return parts[1], dict(zip(MEMORY_FIELDS, parts[2:]))


def main():
    args = parse_args()

    agora = datetime.datetime.now()
    data = args.data or agora.date().isoformat()
    hora = agora.strftime("%H:%M:%S")
    sessao = agora.strftime("%Y%m%dT%H%M%S")
    cenario = prompt_if_missing(args.cenario, "Cenario (ex.: baseline, alcance-10m)")
    distancia = prompt_if_missing(args.distancia, "Distancia em metros (vazio se N/A)", "")
    obstaculo = prompt_if_missing(args.obstaculo, "Obstaculo (ex.: nenhum, 1 parede)", "nenhum")

    metadata = {
        "data": data,
        "hora": hora,
        "sessao": sessao,
        "repeticao": 0,
        "cenario": cenario,
        "distancia_m": distancia,
        "obstaculo": obstaculo,
        "observacoes": args.observacoes,
    }

    out_path = args.out or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "resultados", "{}.csv".format(data)
    )
    out_path = os.path.abspath(out_path)

    if args.port == "auto":
        port = detect_station_a_port(args.baud)
        if port is None:
            sys.stderr.write(
                "Nao encontrei a estacao A em nenhuma porta serial.\n"
                "Confira se a placa esta ligada e, se precisar, passe a porta na mao:\n"
                "    --port /dev/ttyACM0\n"
            )
            sys.exit(1)
        print("Estacao A encontrada em {}".format(port))
    else:
        port = args.port

    is_new = ensure_csv_header(out_path)
    print("Gravando resultados em: {}{}".format(out_path, " (novo arquivo)" if is_new else ""))
    print("Cenario: {} | distancia_m={} | obstaculo={}".format(cenario, distancia or "N/A", obstaculo))
    print("Conectando em {} @ {} baud... (Ctrl+C para sair)".format(port, args.baud))
    print("-" * 60)

    # O USB nativo do ESP32-C6 mapeia DTR/RTS para o reset e para o boot mode
    # do chip. Abrir a porta com esses sinais acionados - o padrao do
    # pyserial - pode deixar a placa presa em reset ou parada no bootloader,
    # sem imprimir absolutamente nada. open_serial() abre com os dois
    # desligados, que e o estado "so escutar".
    ser = open_serial(port, args.baud)

    # A saida que importa (Station A ready/LINKSETUP/MEMORY) sai uma vez so,
    # no setup() da placa. Se ela ja tinha bootado antes deste script
    # conectar, a tela fica vazia sem que nada esteja errado - dai o aviso.
    print(">> Conectado. Se nada aparecer em alguns segundos:")
    print(">>   - aperte RESET na estacao A para ver o boot desde o inicio, ou")
    print(">>   - digite 'r' + Enter para repetir o benchmark sem resetar.")
    print("-" * 60)

    session_fields = {}
    rows_written = 0

    try:
        while True:
            # Repassa o que o usuario digitar para a placa (ex.: "r" + Enter),
            # do mesmo jeito que o Monitor Serial do Arduino faria.
            if select.select([sys.stdin], [], [], 0)[0]:
                typed = sys.stdin.readline()
                if typed:
                    ser.write(typed.encode("utf-8", errors="ignore"))

            raw = ser.readline()
            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line:
                continue

            print(line)

            # Cada "r" no firmware inicia um novo lote na mesma sessao/boot.
            # Guardar a repeticao permite calcular variabilidade entre execucoes
            # sem confundir uma repeticao com um novo custo de memoria/link.
            if line.startswith("=== Starting ") and " benchmark ===" in line:
                metadata["repeticao"] = int(metadata.get("repeticao", 0)) + 1
                print(">> repeticao {} iniciada".format(metadata["repeticao"]))
                continue

            if line.startswith("LINKSETUP,"):
                tech, ms = parse_linksetup_line(line)
                if ms is not None:
                    session_fields["link_setup_ms"] = ms
                    print(">> link_setup_ms capturado para {}: {} ms".format(tech, ms))
                continue

            if line.startswith("MEMORY,"):
                tech, fields = parse_memory_line(line)
                if fields is None:
                    print(">> AVISO: linha MEMORY com formato inesperado, ignorada.")
                    continue

                session_fields.update(fields)
                print(">> custo de memoria capturado para {}: {} bytes no radio + {} bytes no enlace".format(
                    tech, fields["custo_radio_bytes"], fields["custo_link_bytes"]
                ))
                continue

            if line.startswith("RESULT,"):
                fields = parse_result_line(line)
                if fields is None:
                    continue  # cabecalho da tabela ou linha truncada

                # Se a captura comecou no meio do benchmark e perdeu a linha
                # "=== Starting ... ===", ainda marcamos o primeiro lote como 1.
                if int(metadata.get("repeticao", 0)) == 0:
                    metadata["repeticao"] = 1

                append_result_row(out_path, metadata, fields, session_fields)
                rows_written += 1
                print(">> linha {} gravada ({} - {} bytes)".format(
                    rows_written, fields["tecnologia"], fields["payload_bytes"]
                ))

    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        print()
        print("Encerrado. {} linha(s) de resultado gravada(s) em {}".format(rows_written, out_path))


if __name__ == "__main__":
    main()
