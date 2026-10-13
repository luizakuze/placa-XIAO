#!/bin/bash

# Script para rodar a dashboard

echo "=================================="
echo "Dashboard de Desempenho"
echo "=================================="
echo ""

cd "$(dirname "$0")"

# Verificar se venv existe
if [ ! -d "venv" ]; then
    echo "Criando ambiente virtual..."
    python3 -m venv venv
    echo "✓ Ambiente criado"
    echo ""
fi

# Ativar venv
echo "Ativando ambiente..."
source venv/bin/activate

# Verificar dependências
echo "Verificando dependências..."
pip install -q -r requirements.txt 2>/dev/null
echo "✓ Dependências OK"
echo ""
echo "Iniciando dashboard..."
echo "Acesse: http://localhost:5000"
echo ""

# Rodar o app
python3 run.py
