"""
Boletim do dia para os voluntarios, com IA generativa (opcional).

Le o historico.csv gravado pela ponte, calcula os numeros de cada geladeira
(temperatura, tempo em alerta, ocupacao e o comportamento de uso da porta)
e transforma isso num aviso curto, em portugues simples, para o grupo de
voluntarios.

Divisao de papeis, de proposito:
  - As DECISOES (o que e critico, o que e lotado) sao das regras do ESP32 e
    da API, que sao testadas e previsiveis.
  - A IA generativa so ESCREVE o texto a partir dos numeros ja calculados.
    Ela nao recebe dado pessoal nenhum e nao decide se um alimento e seguro.

Sem ANTHROPIC_API_KEY no .env, o boletim e montado por um modelo de texto
fixo, entao o script sempre funciona.

Como rodar:
    python resumo_ia.py              (usa a IA se houver chave)
    python resumo_ia.py --sem-ia     (forca o texto fixo)
"""
import csv
import os
import sys
from collections import defaultdict
from pathlib import Path

import requests

from ponte_mqtt_api import ARQUIVO_CSV, carregar_env, PASTA

carregar_env(PASTA / ".env")
MODELO = os.getenv("MODELO_IA", "claude-haiku-5-5")


def numero(valor):
    try:
        return float(valor)
    except (TypeError, ValueError):
        return None


def calcular_indicadores(caminho: Path) -> dict:
    """Agrupa o historico por geladeira e calcula os indicadores."""
    por_geladeira = defaultdict(lambda: {"temps": [], "niveis": [], "ocupacoes": [], "usos": []})
    with caminho.open(encoding="utf-8") as f:
        for linha in csv.DictReader(f):
            g = por_geladeira[linha["geladeiraId"]]
            if linha["tipo"] == "telemetria":
                t = numero(linha["temperaturaC"])
                if t is not None:
                    g["temps"].append(t)
                g["niveis"].append(linha["nivel"])
                oc = numero(linha["ocupacaoPct"])
                if oc is not None:
                    g["ocupacoes"].append(oc)
            elif linha["tipo"] == "uso":
                g["usos"].append(linha)

    resultado = {}
    for gid, g in sorted(por_geladeira.items()):
        n = len(g["niveis"]) or 1
        aberturas = sum(int(u["aberturas"] or 0) for u in g["usos"])
        esquecimentos = sum(int(u["esquecimentos"] or 0) for u in g["usos"])
        pico = max(g["usos"], key=lambda u: int(u["aberturas"] or 0), default=None)
        resultado[gid] = {
            "leituras": len(g["niveis"]),
            "temp_media": round(sum(g["temps"]) / len(g["temps"]), 1) if g["temps"] else None,
            "temp_max": max(g["temps"]) if g["temps"] else None,
            "pct_critico": round(100 * g["niveis"].count("CRITICO") / n),
            "pct_atencao": round(100 * g["niveis"].count("ATENCAO") / n),
            "ocupacao_atual": g["ocupacoes"][-1] if g["ocupacoes"] else None,
            "aberturas": aberturas,
            "esquecimentos": esquecimentos,
            "pico": pico["dataHora"][11:16] if pico and int(pico["aberturas"] or 0) > 0 else None,
        }
    return resultado


def boletim_por_regras(ind: dict) -> str:
    """Texto fixo, usado quando nao ha IA disponivel."""
    linhas = ["Boletim das geladeiras solidarias", ""]
    for gid, i in ind.items():
        linhas.append(f"Geladeira {gid}:")
        if i["pct_critico"] > 0:
            linhas.append(f"  - URGENTE: ficou em estado critico em {i['pct_critico']}% das leituras "
                          f"(maxima de {i['temp_max']} C). Confira a refrigeracao e os alimentos refrigerados.")
        elif i["temp_media"] is not None:
            linhas.append(f"  - Temperatura media de {i['temp_media']} C, sem periodo critico.")
        if i["ocupacao_atual"] is not None:
            oc = i["ocupacao_atual"]
            situacao = "lotada, leve doacoes para outra geladeira" if oc >= 90 else \
                       "quase vazia, precisa de reposicao" if oc <= 20 else "com espaco"
            linhas.append(f"  - Ocupacao atual: {oc:.0f}% ({situacao}).")
        if i["aberturas"]:
            pico = f", mais movimento por volta das {i['pico']}" if i["pico"] else ""
            linhas.append(f"  - Porta aberta {i['aberturas']} vezes{pico}.")
        if i["esquecimentos"]:
            linhas.append(f"  - A porta ficou esquecida aberta {i['esquecimentos']} vez(es): "
                          f"vale reforcar o aviso 'feche a porta' no local.")
        linhas.append("")
    return "\n".join(linhas).strip()


def boletim_com_ia(ind: dict, chave: str) -> str:
    """Pede ao modelo so a redacao do aviso, a partir dos numeros ja calculados."""
    dados = "\n".join(f"Geladeira {gid}: {i}" for gid, i in ind.items())
    prompt = (
        "Voce escreve avisos para voluntarios de geladeiras solidarias (doacao de alimentos) "
        "em Sao Paulo. Com base SOMENTE nos indicadores abaixo, escreva um aviso curto em "
        "portugues simples, para mandar no grupo de mensagens. Comece pelo que e urgente "
        "(pct_critico > 0), depois reposicao (ocupacao <= 20) e lotacao (ocupacao >= 90), e por "
        "fim o comportamento de uso (pico de movimento, porta esquecida aberta). "
        "Nao invente numeros nem fatos que nao estejam nos indicadores. Maximo de 120 palavras.\n\n"
        f"Indicadores:\n{dados}"
    )
    r = requests.post(
        "https://api.anthropic.com/v1/messages",
        headers={"x-api-key": chave, "anthropic-version": "2023-06-01", "content-type": "application/json"},
        json={"model": MODELO, "max_tokens": 400, "messages": [{"role": "user", "content": prompt}]},
        timeout=30,
    )
    r.raise_for_status()
    return "".join(b.get("text", "") for b in r.json().get("content", [])).strip()


def main() -> None:
    if not ARQUIVO_CSV.exists():
        print(f"Ainda nao ha historico ({ARQUIVO_CSV.name}). Rode a ponte com o Wokwi ligado primeiro.")
        sys.exit(1)
    ind = calcular_indicadores(ARQUIVO_CSV)
    if not ind:
        print("O historico esta vazio.")
        sys.exit(1)

    print("Indicadores calculados:")
    for gid, i in ind.items():
        print(f"  geladeira {gid}: {i}")
    print()

    chave = os.getenv("ANTHROPIC_API_KEY", "")
    if chave and "--sem-ia" not in sys.argv:
        try:
            texto = boletim_com_ia(ind, chave)
            print(f"Boletim escrito pela IA ({MODELO}):\n")
            print(texto)
            return
        except requests.RequestException as e:
            print(f"(IA indisponivel: {e.__class__.__name__}; usando o texto fixo)\n")
    print(boletim_por_regras(ind))


if __name__ == "__main__":
    main()
