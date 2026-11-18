"""
Ponte MQTT -> API Java da Geladeira Solidaria Inteligente.

O ESP32 publica no broker MQTT; esta ponte assina os topicos das geladeiras,
mostra tudo no terminal, grava um historico em CSV (usado pelo resumo com IA)
e encaminha cada telemetria para a API Java em POST /api/leituras, usando a
conta do papel SENSOR.

Por que uma ponte e nao o ESP32 chamando a API direto?
  - O ESP32 so precisa falar MQTT, que e leve e aguenta rede ruim.
  - A senha da API fica num computador, nao gravada em cada geladeira.
  - Se a API cair, as geladeiras continuam publicando normalmente.

Como rodar:
    pip install -r requirements.txt
    copy .env.example .env      (Windows)   |   cp .env.example .env
    python ponte_mqtt_api.py
"""
import csv
import json
import os
import sys
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt
import requests

PASTA = Path(__file__).resolve().parent


def carregar_env(caminho: Path) -> None:
    """Le um .env simples (CHAVE=valor) sem precisar de biblioteca extra."""
    if not caminho.exists():
        return
    for linha in caminho.read_text(encoding="utf-8").splitlines():
        linha = linha.strip()
        if not linha or linha.startswith("#") or "=" not in linha:
            continue
        chave, valor = linha.split("=", 1)
        os.environ.setdefault(chave.strip(), valor.strip())


carregar_env(PASTA / ".env")

MQTT_BROKER = os.getenv("MQTT_BROKER", "broker.hivemq.com")
MQTT_PORTA = int(os.getenv("MQTT_PORTA", "1883"))
TOPICO_RAIZ = os.getenv("TOPICO_RAIZ", "geladeira-solidaria/rm567693/geladeira")
ENVIAR_API = os.getenv("ENVIAR_API", "true").lower() == "true"
API_URL = os.getenv("API_URL", "http://localhost:8080").rstrip("/")
API_EMAIL = os.getenv("API_EMAIL", "sensor@geladeira.test")
API_SENHA = os.getenv("API_SENHA", "")
ARQUIVO_CSV = PASTA / os.getenv("ARQUIVO_CSV", "historico.csv")

CAMPOS_CSV = ["dataHora", "geladeiraId", "tipo", "temperaturaC", "umidadePct", "portaAberta",
              "pesoKg", "ocupacaoPct", "nivel", "motivo", "aberturas", "mediaAberturaSeg",
              "esquecimentos", "movimento"]


def agora() -> str:
    return datetime.now().strftime("%H:%M:%S")


def log(icone: str, texto: str) -> None:
    print(f"{agora()} {icone} {texto}", flush=True)


# ------------------------------- API Java -------------------------------
class ClienteApi:
    """Faz login como SENSOR e envia as leituras. Refaz o login quando o token vence."""

    def __init__(self) -> None:
        self.token = None
        self.pendentes = deque(maxlen=100)   # leituras que nao chegaram (API fora do ar)

    def login(self) -> bool:
        try:
            r = requests.post(f"{API_URL}/auth/login",
                              json={"email": API_EMAIL, "senha": API_SENHA}, timeout=5)
        except requests.RequestException as e:
            log("!!", f"API fora do ar ({e.__class__.__name__}); guardo as leituras e tento depois")
            return False
        if r.status_code != 200:
            log("!!", f"login na API falhou ({r.status_code}): confira API_EMAIL e API_SENHA no .env")
            return False
        self.token = r.json().get("token")
        log("OK", f"login na API como {API_EMAIL}")
        return True

    def enviar_leitura(self, leitura: dict) -> None:
        self.pendentes.append(leitura)
        self._descarregar()

    def _descarregar(self) -> None:
        relogins = 0
        while self.pendentes:
            if not self.token and not self.login():
                return
            leitura = self.pendentes[0]
            try:
                r = requests.post(f"{API_URL}/api/leituras", json=leitura, timeout=5,
                                  headers={"Authorization": f"Bearer {self.token}"})
            except requests.RequestException:
                log("!!", f"API fora do ar; {len(self.pendentes)} leitura(s) aguardando")
                return
            if r.status_code == 401:          # token venceu: faz login de novo (uma vez)
                self.token = None
                relogins += 1
                if relogins > 1:
                    log("!!", "API continua recusando o token; tento de novo na proxima leitura")
                    return
                continue
            self.pendentes.popleft()
            if r.status_code in (200, 201):
                corpo = r.json()
                extra = "  -> API ABRIU ALERTA DE TEMPERATURA" if corpo.get("alertaGerado") else ""
                log("->", f"API: leitura #{corpo.get('id')} da geladeira {leitura['geladeiraId']} "
                          f"salva ({leitura['temperaturaC']} C){extra}")
            else:
                log("!!", f"API recusou a leitura ({r.status_code}): {r.text[:200]}")


api = ClienteApi()


# ------------------------------ Historico -------------------------------
def gravar_csv(linha: dict) -> None:
    novo = not ARQUIVO_CSV.exists()
    with ARQUIVO_CSV.open("a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=CAMPOS_CSV, extrasaction="ignore")
        if novo:
            w.writeheader()
        w.writerow({"dataHora": datetime.now().isoformat(timespec="seconds"), **linha})


# ------------------------------ Mensagens -------------------------------
def tratar_telemetria(d: dict) -> None:
    log("  ", f"geladeira {d['geladeiraId']}: {d['temperaturaC']} C, {d['umidadePct']}% umid., "
              f"porta {'ABERTA' if d['portaAberta'] else 'fechada'}, {d['pesoKg']} kg "
              f"({d['ocupacaoPct']}%), {d['nivel']}")
    gravar_csv({**d, "tipo": "telemetria"})
    if not d.get("sensorOk", True):
        log("!!", "sensor sem leitura valida: nao envio para a API")
        return
    if ENVIAR_API:
        api.enviar_leitura({
            "geladeiraId": d["geladeiraId"],
            "temperaturaC": d["temperaturaC"],
            "umidadePct": d["umidadePct"],
            "portaAberta": d["portaAberta"],
        })


def tratar_alerta(d: dict) -> None:
    icone = {"CRITICO": "XX", "ATENCAO": "!!", "INFO": "i ", "OK": "OK"}.get(d["nivel"], "  ")
    log(icone, f"ALERTA geladeira {d['geladeiraId']}: {d['nivel']} / {d['motivo']} - {d['mensagem']}")


def tratar_uso(d: dict) -> None:
    log("IoB", f"geladeira {d['geladeiraId']}: {d['aberturas']} aberturas em {d['janelaMin']} min "
               f"(~{d['aberturasPorHora']}/h, {d['movimento']}), media {d['mediaAberturaSeg']} s, "
               f"{d['esquecimentos']} esquecida(s) aberta")
    gravar_csv({**d, "tipo": "uso"})


def tratar_porta(d: dict) -> None:
    if d["evento"] == "ABRIU":
        log("  ", f"geladeira {d['geladeiraId']}: porta abriu")
    else:
        log("  ", f"geladeira {d['geladeiraId']}: porta fechou depois de {d['duracaoSeg']} s")


def tratar_status(id_geladeira: str, d: dict) -> None:
    log("OK" if d.get("online") else "XX",
        f"geladeira {id_geladeira} {'ONLINE' if d.get('online') else 'OFFLINE (caiu sem avisar)'}")


TRATADORES = {"telemetria": tratar_telemetria, "alerta": tratar_alerta,
              "uso": tratar_uso, "porta": tratar_porta}


def ao_conectar(cliente, _userdata, _flags, codigo, _props=None):
    if codigo != 0:
        log("!!", f"broker recusou a conexao (codigo {codigo})")
        return
    cliente.subscribe(f"{TOPICO_RAIZ}/+/#", qos=1)
    log("OK", f"conectado em {MQTT_BROKER}, assinando {TOPICO_RAIZ}/+/#")


def ao_receber(_cliente, _userdata, msg):
    partes = msg.topic.split("/")
    tipo, id_geladeira = partes[-1], partes[-2]
    try:
        dados = json.loads(msg.payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        log("!!", f"mensagem invalida em {msg.topic}: {msg.payload[:80]!r}")
        return
    try:
        if tipo == "status":
            tratar_status(id_geladeira, dados)
        elif tipo in TRATADORES:
            TRATADORES[tipo](dados)
    except KeyError as e:
        log("!!", f"campo {e} faltando em {msg.topic}")


def main() -> None:
    if ENVIAR_API and not API_SENHA:
        print("Defina API_SENHA no arquivo .env (copie de .env.example) ou use ENVIAR_API=false.")
        sys.exit(1)
    log("  ", f"API: {'enviando para ' + API_URL if ENVIAR_API else 'desligada (ENVIAR_API=false)'}")
    log("  ", f"historico em {ARQUIVO_CSV.name}")

    cliente = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"ponte-geladeira-{os.getpid()}")
    cliente.on_connect = ao_conectar
    cliente.on_message = ao_receber
    cliente.reconnect_delay_set(min_delay=1, max_delay=30)
    cliente.connect(MQTT_BROKER, MQTT_PORTA, keepalive=30)
    try:
        cliente.loop_forever()
    except KeyboardInterrupt:
        log("  ", "ponte encerrada")


if __name__ == "__main__":
    main()
