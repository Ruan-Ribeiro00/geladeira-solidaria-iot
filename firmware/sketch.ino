// =====================================================================
//  Geladeira Solidaria Inteligente - no IoT da geladeira (ESP32)
//  Global Solution 2026/2 - CitizenTech - FIAP 2TDSPA
//  Disciplina: Disruptive Architectures: IoT, IoB & Generative IA
//
//  O que este codigo faz, a cada 2 segundos:
//    1. Le temperatura e umidade (DHT22), a porta (sensor magnetico,
//       simulado por uma chave deslizante) e o peso dos alimentos
//       (celula de carga + HX711).
//    2. Decide o nivel da geladeira (OK / INFO / ATENCAO / CRITICO) com
//       as regras de regras.h e acende o LED da cor certa, toca o buzzer
//       quando precisa de alguem la e mostra tudo no LCD.
//    3. Publica no broker MQTT: telemetria a cada 10 s, alertas quando o
//       nivel muda, eventos de porta na hora e o resumo de uso (IoB).
//    4. Escuta comandos remotos (silenciar, mudar intervalo, tarar balanca).
//
//  Sem Wi-Fi ou sem broker, a geladeira continua funcionando sozinha
//  (LEDs, buzzer e LCD) e guarda as ultimas mensagens para mandar depois.
// =====================================================================
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <HX711.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "regras.h"

// ----------------------------- Configuracao --------------------------
// Wi-Fi do simulador Wokwi (sem senha). Na geladeira real, troque pela
// rede do local onde ela esta instalada.
const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_SENHA = "";

// Broker MQTT publico, so para o prototipo (veja "Limitacoes" no README).
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORTA  = 1883;

// Id da geladeira na API Java (3 = Geladeira Solidaria Heliopolis).
const int GELADEIRA_ID = 3;

// Capacidade usada para calcular a ocupacao. A celula de carga do
// simulador vai ate 50 kg; numa geladeira real seria a capacidade dela.
const float CAPACIDADE_KG = 50.0;

// Calibracao da celula de carga de 50 kg do Wokwi: leitura bruta de
// 0 a 21000 para 0 a 50 kg  ->  21000 / 50 = 420 unidades por kg.
const float ESCALA_HX711 = 420.0;

// Tempos
const unsigned long LEITURA_MS     = 2000;           // le os sensores
unsigned long       telemetriaMs   = 10000;          // publica telemetria (muda por comando)
const unsigned long JANELA_USO_MS  = 2UL * 60000UL;  // resumo de uso a cada 2 min (demo)
const unsigned long SILENCIO_MS    = 5UL * 60000UL;  // botao silencia o buzzer por 5 min
const unsigned long RECONEXAO_MS   = 5000;           // tenta reconectar a cada 5 s
const unsigned long PAGINA_LCD_MS  = 4000;           // troca a 2a linha do LCD

// ------------------------------- Pinos -------------------------------
const int PIN_DHT       = 15;
const int PIN_HX_DT     = 16;
const int PIN_HX_SCK    = 4;
const int PIN_PORTA     = 27;  // chave: ligada ao GND = porta FECHADA
const int PIN_BOTAO     = 14;  // botao do voluntario: silenciar buzzer
const int PIN_LED_VERDE = 26;
const int PIN_LED_AMAR  = 25;
const int PIN_LED_VERM  = 33;
const int PIN_BUZZER    = 32;
// LCD I2C: SDA = GPIO 21, SCL = GPIO 22 (padrao do ESP32)

// ------------------------------ Objetos ------------------------------
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
DHTesp dht;
HX711 balanca;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Topicos: geladeira-solidaria/rm567693/geladeira/3/<tipo>
// O RM no caminho evita misturar com outros projetos no broker publico.
const char* TOPICO_RAIZ = "geladeira-solidaria/rm567693/geladeira";
char topTelemetria[80], topAlerta[80], topPorta[80], topUso[80], topStatus[80], topComando[80];

// ------------------------------ Estado -------------------------------
float temperaturaC = 0, umidadePct = 0, pesoKg = 0, ocupacaoPct = 0;
bool  sensorOk = false;
int   leiturasQuentes = 0;

bool  portaAberta = false;
unsigned long portaAbertaDesde = 0;     // quando abriu (para o alerta)
unsigned long portaContadaDesde = 0;    // desde quando o tempo aberto entra na janela de uso
int   leituraPortaAnterior = LOW;
unsigned long portaMudouEm = 0;

Avaliacao avaliacao = { NIVEL_OK, MOTIVO_NENHUM };
Avaliacao avaliacaoPublicada = { NIVEL_OK, MOTIVO_NENHUM };
bool  primeiraAvaliacao = true;

unsigned long silenciadoAte = 0;
UsoPorta uso = { 0, 0, 0 };
unsigned long seq = 0;

unsigned long ultimaLeitura = 0, ultimaTelemetria = 0, ultimaJanelaUso = 0;
unsigned long ultimaTentativaWifi = 0, ultimaTentativaMqtt = 0, ultimaPaginaLcd = 0;
int paginaLcd = 0;

// --------------- Fila offline (mensagens sem conexao) ----------------
// Guarda ate 15 mensagens; se encher, descarta a mais antiga.
const int FILA_MAX = 15;
char filaTopico[FILA_MAX][80];
char filaMsg[FILA_MAX][300];
int  filaInicio = 0, filaTamanho = 0;

void enfileirar(const char* topico, const char* msg) {
  int pos = (filaInicio + filaTamanho) % FILA_MAX;
  if (filaTamanho == FILA_MAX) {               // cheia: perde a mais antiga
    filaInicio = (filaInicio + 1) % FILA_MAX;
    pos = (filaInicio + filaTamanho - 1) % FILA_MAX;
  } else {
    filaTamanho++;
  }
  strncpy(filaTopico[pos], topico, sizeof(filaTopico[pos]) - 1);
  filaTopico[pos][sizeof(filaTopico[pos]) - 1] = '\0';
  strncpy(filaMsg[pos], msg, sizeof(filaMsg[pos]) - 1);
  filaMsg[pos][sizeof(filaMsg[pos]) - 1] = '\0';
}

void esvaziarFila() {
  int enviadas = 0;
  while (filaTamanho > 0 && mqtt.connected()) {
    if (!mqtt.publish(filaTopico[filaInicio], filaMsg[filaInicio])) break;
    filaInicio = (filaInicio + 1) % FILA_MAX;
    filaTamanho--;
    enviadas++;
  }
  if (enviadas > 0) {
    Serial.printf("[MQTT] %d mensagem(ns) guardada(s) offline foram enviadas\n", enviadas);
  }
}

// Publica; se nao der, guarda na fila para mandar quando reconectar.
void publicar(const char* topico, const char* msg, bool guardarSeFalhar = true) {
  if (mqtt.connected() && mqtt.publish(topico, msg)) {
    Serial.printf("[MQTT] -> %s %s\n", topico, msg);
    return;
  }
  if (guardarSeFalhar) {
    enfileirar(topico, msg);
    Serial.printf("[MQTT] offline, guardado na fila (%d/%d)\n", filaTamanho, FILA_MAX);
  }
}

// ------------------------------ Conexao ------------------------------
void tratarComando(char* topico, byte* payload, unsigned int tamanho);

void conectarWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - ultimaTentativaWifi < RECONEXAO_MS && ultimaTentativaWifi != 0) return;
  ultimaTentativaWifi = millis();
  Serial.println("[WiFi] conectando...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);   // canal 6: conecta mais rapido no Wokwi
}

void conectarMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  if (millis() - ultimaTentativaMqtt < RECONEXAO_MS && ultimaTentativaMqtt != 0) return;
  ultimaTentativaMqtt = millis();

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "geladeira-%d-%04lx", GELADEIRA_ID, (unsigned long)random(0xffff));
  Serial.printf("[MQTT] conectando em %s como %s...\n", MQTT_BROKER, clientId);

  // "Last Will": se a geladeira cair sem avisar, o broker publica "offline".
  if (mqtt.connect(clientId, topStatus, 1, true, "{\"online\":false}")) {
    Serial.println("[MQTT] conectado");
    mqtt.publish(topStatus, "{\"online\":true}", true);
    mqtt.subscribe(topComando);
    esvaziarFila();
  } else {
    Serial.printf("[MQTT] falhou (codigo %d), tento de novo em 5 s\n", mqtt.state());
  }
}

// --------------------------- Leitura e regra --------------------------
void lerSensores() {
  TempAndHumidity d = dht.getTempAndHumidity();
  sensorOk = (dht.getStatus() == 0) && !isnan(d.temperature) && !isnan(d.humidity);
  if (sensorOk) {
    temperaturaC = d.temperature;
    umidadePct = d.humidity;
  }

  if (balanca.wait_ready_timeout(200)) {
    pesoKg = balanca.get_units(3);
    if (pesoKg < 0) pesoKg = 0;      // ruido perto de zero
  }
  ocupacaoPct = calcularOcupacao(pesoKg, CAPACIDADE_KG);

  leiturasQuentes = atualizarLeiturasQuentes(leiturasQuentes, temperaturaC, sensorOk);
}

unsigned long tempoPortaAberta() {
  return portaAberta ? millis() - portaAbertaDesde : 0;
}

void montarTelemetria(char* buf, size_t tam) {
  snprintf(buf, tam,
    "{\"geladeiraId\":%d,\"seq\":%lu,\"sensorOk\":%s,\"temperaturaC\":%.1f,\"umidadePct\":%.1f,"
    "\"portaAberta\":%s,\"portaAbertaSeg\":%lu,\"pesoKg\":%.1f,\"ocupacaoPct\":%.1f,"
    "\"nivel\":\"%s\",\"motivo\":\"%s\",\"uptimeSeg\":%lu,\"rssi\":%d}",
    GELADEIRA_ID, ++seq, sensorOk ? "true" : "false", temperaturaC, umidadePct,
    portaAberta ? "true" : "false", tempoPortaAberta() / 1000, pesoKg, ocupacaoPct,
    nomeNivel(avaliacao.nivel), nomeMotivo(avaliacao.motivo), millis() / 1000,
    WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0);
}

void publicarTelemetria() {
  char buf[300];
  montarTelemetria(buf, sizeof(buf));
  publicar(topTelemetria, buf);
}

void publicarAlerta() {
  char buf[300];
  const char* msg = avaliacao.nivel == NIVEL_OK ? "Geladeira voltou ao normal" : textoLcd(avaliacao.motivo);
  snprintf(buf, sizeof(buf),
    "{\"geladeiraId\":%d,\"nivel\":\"%s\",\"motivo\":\"%s\",\"mensagem\":\"%s\","
    "\"temperaturaC\":%.1f,\"ocupacaoPct\":%.1f,\"portaAbertaSeg\":%lu}",
    GELADEIRA_ID, nomeNivel(avaliacao.nivel), nomeMotivo(avaliacao.motivo), msg,
    temperaturaC, ocupacaoPct, tempoPortaAberta() / 1000);
  publicar(topAlerta, buf);
}

void avaliarEReagir() {
  avaliacao = avaliar(sensorOk, temperaturaC, umidadePct, leiturasQuentes,
                      portaAberta, tempoPortaAberta(), ocupacaoPct);

  bool mudou = primeiraAvaliacao ||
               avaliacao.nivel != avaliacaoPublicada.nivel ||
               avaliacao.motivo != avaliacaoPublicada.motivo;
  if (mudou) {
    Serial.printf("[REGRA] %s / %s\n", nomeNivel(avaliacao.nivel), nomeMotivo(avaliacao.motivo));
    // Se surgiu um problema novo, o silencio anterior deixa de valer.
    if (!primeiraAvaliacao && avaliacao.nivel > avaliacaoPublicada.nivel) silenciadoAte = 0;
    publicarAlerta();
    publicarTelemetria();             // manda o estado novo na hora
    avaliacaoPublicada = avaliacao;
    primeiraAvaliacao = false;
  }
}

// --------------------------- Saidas locais ---------------------------
bool estaSilenciado() {
  return silenciadoAte != 0 && (long)(silenciadoAte - millis()) > 0;
}

void atualizarLeds() {
  digitalWrite(PIN_LED_VERDE, avaliacao.nivel <= NIVEL_INFO);
  digitalWrite(PIN_LED_AMAR,  avaliacao.nivel == NIVEL_ATENCAO || avaliacao.nivel == NIVEL_INFO);
  digitalWrite(PIN_LED_VERM,  avaliacao.nivel == NIVEL_CRITICO);
}

// Buzzer sem travar o loop: critico = bipe rapido; porta = bipe curto por segundo.
void atualizarBuzzer() {
  if (!deveTocarBuzzer(avaliacao, estaSilenciado())) {
    noTone(PIN_BUZZER);
    return;
  }
  unsigned long t = millis();
  bool ligado = (avaliacao.nivel == NIVEL_CRITICO) ? (t % 400 < 200) : (t % 1000 < 100);
  if (ligado) tone(PIN_BUZZER, avaliacao.nivel == NIVEL_CRITICO ? 2000 : 1200);
  else noTone(PIN_BUZZER);
}

void atualizarLcd() {
  char l1[17], l2[17];
  if (sensorOk) snprintf(l1, sizeof(l1), "%4.1fC %3.0f%% P:%s", temperaturaC, umidadePct, portaAberta ? "ABR" : "FEC");
  else          snprintf(l1, sizeof(l1), "Sensor sem dados");

  if (paginaLcd == 0) {
    snprintf(l2, sizeof(l2), "%-16s", textoLcd(avaliacao.motivo));
  } else {
    snprintf(l2, sizeof(l2), "%4.1fkg %3.0f%% %s", pesoKg, ocupacaoPct, mqtt.connected() ? "ON " : "OFF");
  }
  lcd.setCursor(0, 0); lcd.print(l1); for (int i = strlen(l1); i < 16; i++) lcd.print(' ');
  lcd.setCursor(0, 1); lcd.print(l2); for (int i = strlen(l2); i < 16; i++) lcd.print(' ');
}

// ------------------------- Porta e IoB (uso) --------------------------
void verificarPorta() {
  int leitura = digitalRead(PIN_PORTA);
  if (leitura != leituraPortaAnterior) {         // debounce de 50 ms
    leituraPortaAnterior = leitura;
    portaMudouEm = millis();
    return;
  }
  if (millis() - portaMudouEm < 50) return;

  // Chave ligada ao GND (LOW) = porta fechada. Fio solto le HIGH = "aberta",
  // entao uma falha no sensor nunca esconde uma porta aberta.
  bool aberta = (leitura == HIGH);
  if (aberta == portaAberta) return;

  portaAberta = aberta;
  char buf[160];
  if (aberta) {
    portaAbertaDesde = millis();
    portaContadaDesde = portaAbertaDesde;
    uso.aberturas++;
    snprintf(buf, sizeof(buf), "{\"geladeiraId\":%d,\"evento\":\"ABRIU\",\"aberturasNaJanela\":%lu}",
             GELADEIRA_ID, uso.aberturas);
  } else {
    unsigned long duracao = millis() - portaAbertaDesde;
    registrarFechamento(uso, millis() - portaContadaDesde, duracao);
    snprintf(buf, sizeof(buf), "{\"geladeiraId\":%d,\"evento\":\"FECHOU\",\"duracaoSeg\":%.1f}",
             GELADEIRA_ID, duracao / 1000.0);
  }
  publicar(topPorta, buf);
}

void publicarResumoUso() {
  // Se a porta esta aberta agora, conta o tempo ate aqui nesta janela.
  UsoPorta janela = uso;
  if (portaAberta) janela.tempoAbertoMs += millis() - portaContadaDesde;

  unsigned long porHora = janela.aberturas * 3600000UL / JANELA_USO_MS;
  char buf[300];
  snprintf(buf, sizeof(buf),
    "{\"geladeiraId\":%d,\"janelaMin\":%lu,\"aberturas\":%lu,\"aberturasPorHora\":%lu,"
    "\"mediaAberturaSeg\":%.1f,\"tempoAbertoSeg\":%lu,\"esquecimentos\":%lu,\"movimento\":\"%s\","
    "\"pesoKg\":%.1f,\"ocupacaoPct\":%.1f}",
    GELADEIRA_ID, JANELA_USO_MS / 60000UL, janela.aberturas, porHora,
    mediaAberturaSeg(janela), janela.tempoAbertoMs / 1000, janela.esquecimentos,
    classificarMovimento(porHora), pesoKg, ocupacaoPct);
  publicar(topUso, buf);

  uso = { 0, 0, 0 };
  // O tempo que ainda falta conta na proxima janela. portaAbertaDesde nao muda,
  // senao o alerta de porta esquecida recomecaria do zero.
  if (portaAberta) portaContadaDesde = millis();
}

// ------------------------------ Comandos -----------------------------
// Exemplos (publicar no topico .../comando):
//   {"acao":"silenciar"}
//   {"acao":"intervalo","segundos":5}
//   {"acao":"tara"}      (zera a balanca com a geladeira vazia)
//   {"acao":"status"}    (manda a telemetria agora)
void tratarComando(char* topico, byte* payload, unsigned int tamanho) {
  char msg[128];
  unsigned int n = tamanho < sizeof(msg) - 1 ? tamanho : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';
  Serial.printf("[CMD] <- %s\n", msg);

  if (strstr(msg, "\"silenciar\"")) {
    silenciadoAte = millis() + SILENCIO_MS;
    Serial.println("[CMD] buzzer silenciado por 5 min");
  } else if (strstr(msg, "\"intervalo\"")) {
    const char* p = strstr(msg, "\"segundos\":");
    int s = p ? atoi(p + 11) : 0;
    if (s >= 2 && s <= 3600) {
      telemetriaMs = (unsigned long)s * 1000UL;
      Serial.printf("[CMD] telemetria a cada %d s\n", s);
    } else {
      Serial.println("[CMD] intervalo invalido (use de 2 a 3600 s)");
    }
  } else if (strstr(msg, "\"tara\"")) {
    balanca.tare();
    Serial.println("[CMD] balanca zerada");
  } else if (strstr(msg, "\"status\"")) {
    publicarTelemetria();
  } else {
    Serial.println("[CMD] comando desconhecido");
  }
}

// --------------------------- setup e loop ----------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Geladeira Solidaria Inteligente ===");

  pinMode(PIN_PORTA, INPUT_PULLUP);
  pinMode(PIN_BOTAO, INPUT_PULLUP);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_AMAR, OUTPUT);
  pinMode(PIN_LED_VERM, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  leituraPortaAnterior = digitalRead(PIN_PORTA);
  portaAberta = (leituraPortaAnterior == HIGH);
  if (portaAberta) { portaAbertaDesde = portaContadaDesde = millis(); uso.aberturas = 1; }

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Geladeira");
  lcd.setCursor(0, 1); lcd.print("Solidaria #"); lcd.print(GELADEIRA_ID);

  dht.setup(PIN_DHT, DHTesp::DHT22);
  balanca.begin(PIN_HX_DT, PIN_HX_SCK);
  balanca.set_scale(ESCALA_HX711);
  // Sem tara automatica: na instalacao real a tara e feita uma vez, com a
  // geladeira vazia, pelo comando {"acao":"tara"}.

  snprintf(topTelemetria, sizeof(topTelemetria), "%s/%d/telemetria", TOPICO_RAIZ, GELADEIRA_ID);
  snprintf(topAlerta,     sizeof(topAlerta),     "%s/%d/alerta",     TOPICO_RAIZ, GELADEIRA_ID);
  snprintf(topPorta,      sizeof(topPorta),      "%s/%d/porta",      TOPICO_RAIZ, GELADEIRA_ID);
  snprintf(topUso,        sizeof(topUso),        "%s/%d/uso",        TOPICO_RAIZ, GELADEIRA_ID);
  snprintf(topStatus,     sizeof(topStatus),     "%s/%d/status",     TOPICO_RAIZ, GELADEIRA_ID);
  snprintf(topComando,    sizeof(topComando),    "%s/%d/comando",    TOPICO_RAIZ, GELADEIRA_ID);

  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setCallback(tratarComando);
  mqtt.setBufferSize(512);
  randomSeed(esp_random());

  WiFi.mode(WIFI_STA);
  conectarWifi();
  Serial.printf("Topicos: %s/%d/<telemetria|alerta|porta|uso|status|comando>\n", TOPICO_RAIZ, GELADEIRA_ID);
  ultimaJanelaUso = millis();
}

void loop() {
  unsigned long agora = millis();

  conectarWifi();
  conectarMqtt();
  mqtt.loop();

  verificarPorta();

  // Botao do voluntario: silencia o buzzer (o LED continua aceso).
  if (digitalRead(PIN_BOTAO) == LOW && !estaSilenciado()) {
    silenciadoAte = agora + SILENCIO_MS;
    Serial.println("[BOTAO] buzzer silenciado por 5 min");
  }

  if (agora - ultimaLeitura >= LEITURA_MS) {
    ultimaLeitura = agora;
    lerSensores();
    avaliarEReagir();
    atualizarLeds();
    atualizarLcd();
  }

  if (agora - ultimaTelemetria >= telemetriaMs) {
    ultimaTelemetria = agora;
    publicarTelemetria();
  }

  if (agora - ultimaJanelaUso >= JANELA_USO_MS) {
    ultimaJanelaUso = agora;
    publicarResumoUso();
  }

  if (agora - ultimaPaginaLcd >= PAGINA_LCD_MS) {
    ultimaPaginaLcd = agora;
    paginaLcd = 1 - paginaLcd;
    atualizarLcd();
  }

  atualizarBuzzer();
  delay(10);
}
