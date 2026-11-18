// Simula o firmware no PC: roda setup()/loop() com tempo e sensores falsos
// e confere o comportamento de cada caso de teste.
#include "Arduino.h"
#include "WiFi.h"
#include "PubSubClient.h"
#include <vector>
#include <string>
#include <cassert>

unsigned long fake_millis = 0;
int fake_pins[64] = {0};
int fake_tone_freq = 0;
FakeSerial Serial;
bool fake_wifi_ok = true;
FakeWiFi WiFi;
bool fake_broker_ok = true;
std::vector<Pub> fake_pubs;
float fake_temp = 4.5, fake_hum = 62; bool fake_dht_ok = true;
long fake_raw = 0;
char fake_lcd[2][17];

#include "../firmware/sketch.ino"

static int falhas = 0, total = 0;
#define CHECK(cond, desc) do { total++; if (cond) std::printf("  [OK]   %s\n", desc); \
  else { falhas++; std::printf("  [FALHA] %s  (linha %d)\n", desc, __LINE__); } } while (0)

void rodar(unsigned long ms) {           // avanca o tempo rodando o loop
  unsigned long fim = fake_millis + ms;
  while (fake_millis < fim) loop();
}
int contarTopico(const char* sufixo, size_t desde = 0) {
  int n = 0;
  for (size_t i = desde; i < fake_pubs.size(); i++)
    if (fake_pubs[i].topic.size() >= strlen(sufixo) &&
        fake_pubs[i].topic.compare(fake_pubs[i].topic.size() - strlen(sufixo), strlen(sufixo), sufixo) == 0) n++;
  return n;
}
const Pub* ultimo(const char* sufixo) {
  for (size_t i = fake_pubs.size(); i-- > 0;)
    if (fake_pubs[i].topic.find(sufixo) != std::string::npos) return &fake_pubs[i];
  return nullptr;
}
bool contem(const Pub* p, const char* trecho) { return p && p->msg.find(trecho) != std::string::npos; }
void comando(const char* json) {
  mqtt.cb((char*)topComando, (byte*)json, strlen(json));
}
bool buzzerTocouEm(unsigned long ms) {
  unsigned long fim = fake_millis + ms; bool tocou = false;
  while (fake_millis < fim) { loop(); if (fake_tone_freq > 0) tocou = true; }
  return tocou;
}

int main() {
  Serial.quiet = true;
  fake_pins[PIN_PORTA] = LOW;     // porta fechada
  fake_pins[PIN_BOTAO] = HIGH;    // botao solto
  fake_raw = (long)(12.5 * 420);  // 12,5 kg na balanca

  std::printf("\nCT01 - Sem broker na partida: guarda na fila e envia ao reconectar\n");
  fake_broker_ok = false;
  setup();
  rodar(25000);
  CHECK(fake_pubs.empty(), "nada publicado enquanto o broker esta fora");
  CHECK(filaTamanho >= 3, "mensagens guardadas na fila offline");
  int naFila = filaTamanho;
  fake_broker_ok = true;
  rodar(6000);
  CHECK(filaTamanho == 0, "fila esvaziada depois de reconectar");
  CHECK(contarTopico("/status") >= 1 && contem(ultimo("/status"), "\"online\":true"), "status online publicado (retained)");
  CHECK((int)fake_pubs.size() >= naFila + 1, "mensagens da fila chegaram ao broker");

  std::printf("\nCT02 - Operacao normal (4,5 C, 62%%, 12,5 kg, porta fechada)\n");
  size_t marca = fake_pubs.size();
  rodar(10500);
  CHECK(contarTopico("/telemetria", marca) >= 1, "telemetria periodica a cada 10 s");
  const Pub* t = ultimo("/telemetria");
  CHECK(contem(t, "\"temperaturaC\":4.5") && contem(t, "\"ocupacaoPct\":25.0") && contem(t, "\"nivel\":\"OK\""), "JSON com temperatura, ocupacao 25% e nivel OK");
  CHECK(fake_pins[PIN_LED_VERDE] == 1 && fake_pins[PIN_LED_AMAR] == 0 && fake_pins[PIN_LED_VERM] == 0, "so o LED verde aceso");
  CHECK(fake_tone_freq == 0, "buzzer desligado");
  CHECK(strncmp(fake_lcd[0], " 4.5C  62% P:FEC", 16) == 0, "LCD linha 1: ' 4.5C  62% P:FEC'");

  std::printf("\nCT03 - Temperatura sobe para 9,5 C (falha na refrigeracao)\n");
  marca = fake_pubs.size();
  fake_temp = 9.5;
  rodar(2100);
  CHECK(avaliacao.nivel == NIVEL_ATENCAO && avaliacao.motivo == MOTIVO_TEMP_SUBINDO, "1a leitura quente: so ATENCAO (pode ser porta aberta)");
  CHECK(fake_pins[PIN_LED_AMAR] == 1 && fake_pins[PIN_LED_VERM] == 0, "LED amarelo");
  rodar(4200);
  CHECK(avaliacao.nivel == NIVEL_CRITICO && avaliacao.motivo == MOTIVO_TEMP_ALTA, "3 leituras seguidas acima de 8 C: CRITICO");
  CHECK(fake_pins[PIN_LED_VERM] == 1 && fake_pins[PIN_LED_VERDE] == 0, "LED vermelho");
  CHECK(contem(ultimo("/alerta"), "\"nivel\":\"CRITICO\"") && contem(ultimo("/alerta"), "\"motivo\":\"TEMPERATURA\""), "alerta CRITICO/TEMPERATURA publicado");
  CHECK(buzzerTocouEm(1000), "buzzer tocando");

  std::printf("\nCT04 - Voluntario aperta o botao Silenciar\n");
  fake_pins[PIN_BOTAO] = LOW; rodar(100); fake_pins[PIN_BOTAO] = HIGH;
  CHECK(!buzzerTocouEm(3000), "buzzer em silencio");
  CHECK(fake_pins[PIN_LED_VERM] == 1, "LED vermelho continua aceso");

  std::printf("\nCT05 - Temperatura volta a 4 C\n");
  fake_temp = 4.0;
  rodar(2100);
  CHECK(avaliacao.nivel == NIVEL_OK, "volta para OK");
  CHECK(contem(ultimo("/alerta"), "voltou ao normal"), "alerta de normalizacao publicado");
  silenciadoAte = 0;

  std::printf("\nCT06 - Porta aberta e esquecida\n");
  marca = fake_pubs.size();
  fake_pins[PIN_PORTA] = HIGH;
  rodar(2100);
  CHECK(contem(ultimo("/porta"), "\"evento\":\"ABRIU\""), "evento ABRIU na hora");
  CHECK(avaliacao.nivel == NIVEL_OK, "aberta ha pouco: ainda OK");
  rodar(60000);
  CHECK(avaliacao.nivel == NIVEL_ATENCAO && avaliacao.motivo == MOTIVO_PORTA_ABERTA, "mais de 60 s: ATENCAO / PORTA_ABERTA");
  CHECK(buzzerTocouEm(2000), "bipe curto para lembrar de fechar");
  CHECK(strncmp(fake_lcd[1], "FECHE A PORTA", 13) == 0 || paginaLcd == 1, "LCD pede para fechar a porta");
  rodar(120000);
  CHECK(avaliacao.nivel == NIVEL_CRITICO && avaliacao.motivo == MOTIVO_PORTA_ABERTA, "mais de 3 min: CRITICO");
  fake_pins[PIN_PORTA] = LOW;
  rodar(2100);
  CHECK(contem(ultimo("/porta"), "\"evento\":\"FECHOU\""), "evento FECHOU com a duracao");
  CHECK(avaliacao.nivel == NIVEL_OK, "porta fechada: volta para OK");

  std::printf("\nCT07 - IoB: resumo de uso da janela\n");
  for (int i = 0; i < 4; i++) {                 // 4 aberturas rapidas de 5 s
    fake_pins[PIN_PORTA] = HIGH; rodar(5000);
    fake_pins[PIN_PORTA] = LOW;  rodar(3000);
  }
  marca = fake_pubs.size();
  rodar(JANELA_USO_MS);
  const Pub* u = ultimo("/uso");
  CHECK(u != nullptr, "resumo de uso publicado a cada janela");
  if (u) std::printf("         %s\n", u->msg.c_str());
  CHECK(contem(u, "\"esquecimentos\":"), "resumo traz aberturas, media e esquecimentos");

  std::printf("\nCT08 - Geladeira lotada (46 kg de 50 kg)\n");
  fake_raw = 46 * 420;
  rodar(2100);
  CHECK(avaliacao.nivel == NIVEL_INFO && avaliacao.motivo == MOTIVO_LOTADA, "92%: INFO / CAPACIDADE");
  CHECK(fake_pins[PIN_LED_VERDE] == 1 && fake_pins[PIN_LED_AMAR] == 1, "verde + amarelo (aviso de logistica)");
  CHECK(fake_tone_freq == 0, "lotacao nao toca buzzer");
  fake_raw = 5 * 420;
  rodar(2100);
  CHECK(avaliacao.motivo == MOTIVO_ESTOQUE_BAIXO, "10%: ESTOQUE_BAIXO");
  fake_raw = (long)(12.5 * 420);
  rodar(2100);

  std::printf("\nCT09 - Comandos remotos pelo MQTT\n");
  comando("{\"acao\":\"intervalo\",\"segundos\":5}");
  CHECK(telemetriaMs == 5000, "intervalo da telemetria mudou para 5 s");
  comando("{\"acao\":\"intervalo\",\"segundos\":0}");
  CHECK(telemetriaMs == 5000, "intervalo invalido e ignorado");
  marca = fake_pubs.size();
  rodar(10100);
  CHECK(contarTopico("/telemetria", marca) >= 2, "telemetria saindo a cada 5 s");
  fake_raw = 300;                                 // ruido com a geladeira vazia
  comando("{\"acao\":\"tara\"}");
  rodar(2100);
  CHECK(pesoKg < 0.01, "tara zera a balanca");
  fake_raw = 300 + 10 * 420;
  rodar(2100);
  CHECK(fabs(pesoKg - 10.0) < 0.05, "depois da tara, 10 kg medidos certo");
  comando("{\"acao\":\"silenciar\"}");
  CHECK(estaSilenciado(), "comando silenciar funciona");
  silenciadoAte = 0;

  std::printf("\nCT10 - Sensor DHT22 para de responder\n");
  fake_dht_ok = false;
  rodar(2100);
  CHECK(avaliacao.nivel == NIVEL_CRITICO && avaliacao.motivo == MOTIVO_SENSOR_FALHA, "CRITICO / SENSOR_FALHA");
  CHECK(contem(ultimo("/telemetria"), "\"sensorOk\":false"), "telemetria avisa sensorOk=false");
  fake_dht_ok = true;
  rodar(2100);

  std::printf("\nCT11 - Wi-Fi cai no meio da operacao\n");
  fake_broker_ok = false;
  size_t antes = fake_pubs.size();
  fake_temp = 9.9;
  rodar(8000);
  CHECK(fake_pubs.size() == antes, "nada sai sem conexao");
  CHECK(fake_pins[PIN_LED_VERM] == 1, "LED e buzzer continuam funcionando sozinhos");
  CHECK(filaTamanho > 0, "alerta guardado na fila");
  fake_broker_ok = true;
  rodar(6000);
  CHECK(filaTamanho == 0 && contem(ultimo("/alerta"), "CRITICO"), "alerta entregue ao reconectar");

  std::printf("\nCT12 - Fila cheia descarta a mais antiga\n");
  fake_broker_ok = false;
  rodar(120000);
  CHECK(filaTamanho == FILA_MAX, "fila nao passa de 15 mensagens");
  fake_broker_ok = true;
  rodar(6000);
  CHECK(filaTamanho == 0, "fila esvaziada");

  // Exporta tudo que foi publicado para validar o JSON em Python
  FILE* f = fopen("publicacoes.txt", "w");
  for (auto& p : fake_pubs) fprintf(f, "%s\t%s\n", p.topic.c_str(), p.msg.c_str());
  fclose(f);

  std::printf("\nResultado: %d de %d verificacoes passaram\n", total - falhas, total);
  return falhas == 0 ? 0 : 1;
}
