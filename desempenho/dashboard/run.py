#!/usr/bin/env python3
"""
Script simples para rodar a dashboard
"""

import sys
import webbrowser
import time
from app import app

if __name__ == '__main__':
    print("""
╔════════════════════════════════════════════════════════════╗
║       Dashboard de Análise de Desempenho                  ║
║                                                            ║
║  Iniciando servidor em http://localhost:5000             ║
║                                                            ║
║  1. Arraste seu CSV na página                            ║
║  2. Aguarde o processamento                              ║
║  3. Veja os gráficos interativos                          ║
║                                                            ║
║  Pressione Ctrl+C para parar                              ║
╚════════════════════════════════════════════════════════════╝
""")

    # Abrir navegador automaticamente
    print("\nAbrindo navegador em 2 segundos...\n")
    time.sleep(2)
    webbrowser.open('http://localhost:5000')

    # Rodar servidor
    try:
        app.run(debug=False, port=5000, host='localhost')
    except KeyboardInterrupt:
        print("\n\nDashboard fechada")
        sys.exit(0)
