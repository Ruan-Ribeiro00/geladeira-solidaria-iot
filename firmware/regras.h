// =====================================================================
//  regras.h - Regras de decisao da Geladeira Solidaria Inteligente
//
//  Aqui fica so a LOGICA (nenhuma chamada de hardware). Por isso estas
//  funcoes podem ser testadas no computador, fora do ESP32
//  (veja testes/test_regras.cpp).
//
//  Os limites batem com a API Java (RN2: acima de 8 C abre alerta
//  critico de temperatura; 90% de ocupacao = lotada; 20% = estoque baixo).
// =====================================================================
#pragma once

// ----------------------- Limites (ajustaveis) ------------------------
const float TEMP_IDEAL_MAX_C      = 5.0;   // ate 5 C: conservacao ideal
const float TEMP_CRITICA_C        = 8.0;   // acima de 8 C: risco a saude (igual a API)
const float TEMP_MINIMA_C         = 0.0;   // abaixo de 0 C: alimento congelando
const float UMIDADE_MAX_PCT       = 85.0;  // acima disso: condensacao / mofo
const float OCUPACAO_LOTADA_PCT   = 90.0;  // igual a API
const float OCUPACAO_BAIXA_PCT    = 20.0;  // igual a API
const unsigned long PORTA_ALERTA_MS  = 60UL * 1000UL;   // porta aberta ha mais de 60 s
const unsigned long PORTA_CRITICA_MS = 180UL * 1000UL;  // ha mais de 3 min
const int LEITURAS_PARA_CRITICO   = 3;     // temperatura alta em 3 leituras seguidas

// Nivel geral da geladeira. A ordem importa: quanto maior, mais grave.
enum Nivel { NIVEL_OK = 0, NIVEL_INFO = 1, NIVEL_ATENCAO = 2, NIVEL_CRITICO = 3 };

// Motivo principal do nivel (o que aparece no LCD e no alerta MQTT).
enum Motivo {
  MOTIVO_NENHUM = 0,
  MOTIVO_SENSOR_FALHA,      // DHT22 sem resposta
  MOTIVO_TEMP_ALTA,         // acima de 8 C de forma sustentada
  MOTIVO_PORTA_ABERTA,      // porta esquecida aberta
  MOTIVO_TEMP_SUBINDO,      // entre 5 e 8 C, ou acima de 8 C ha pouco tempo
  MOTIVO_CONGELANDO,        // abaixo de 0 C
  MOTIVO_UMIDADE_ALTA,      // acima de 85 %
  MOTIVO_LOTADA,            // ocupacao >= 90 %
  MOTIVO_ESTOQUE_BAIXO      // ocupacao <= 20 %
};

struct Avaliacao {
  Nivel nivel;
  Motivo motivo;
};

// Ocupacao em % a partir do peso medido pela celula de carga.
// Peso negativo (ruido da balanca) conta como zero.
inline float calcularOcupacao(float pesoKg, float capacidadeKg) {
  if (capacidadeKg <= 0) return 0;
  if (pesoKg < 0) pesoKg = 0;
  float pct = pesoKg * 100.0f / capacidadeKg;
  return pct > 100.0f ? 100.0f : pct;
}

// Conta leituras quentes seguidas. Uma leitura normal zera o contador.
// Isso evita alarme falso: quando alguem abre a porta, a temperatura
// sobe por alguns instantes e volta ao normal.
inline int atualizarLeiturasQuentes(int contadorAtual, float tempC, bool sensorOk) {
  if (!sensorOk) return contadorAtual;          // falha de leitura nao conta nem zera
  if (tempC > TEMP_CRITICA_C) return contadorAtual + 1;
  return 0;
}

// Decide o nivel e o motivo, do mais grave para o menos grave.
// Temperatura vem antes de lotacao porque e risco a saude; lotacao e logistica
// (mesma prioridade da RN1 da API).
inline Avaliacao avaliar(bool sensorOk, float tempC, float umidadePct,
                         int leiturasQuentesSeguidas,
                         bool portaAberta, unsigned long portaAbertaHaMs,
                         float ocupacaoPct) {
  if (!sensorOk) return { NIVEL_CRITICO, MOTIVO_SENSOR_FALHA };
  if (leiturasQuentesSeguidas >= LEITURAS_PARA_CRITICO) return { NIVEL_CRITICO, MOTIVO_TEMP_ALTA };
  if (portaAberta && portaAbertaHaMs >= PORTA_CRITICA_MS) return { NIVEL_CRITICO, MOTIVO_PORTA_ABERTA };
  if (portaAberta && portaAbertaHaMs >= PORTA_ALERTA_MS)  return { NIVEL_ATENCAO, MOTIVO_PORTA_ABERTA };
  if (tempC > TEMP_IDEAL_MAX_C)    return { NIVEL_ATENCAO, MOTIVO_TEMP_SUBINDO };
  if (tempC < TEMP_MINIMA_C)       return { NIVEL_ATENCAO, MOTIVO_CONGELANDO };
  if (umidadePct > UMIDADE_MAX_PCT) return { NIVEL_ATENCAO, MOTIVO_UMIDADE_ALTA };
  if (ocupacaoPct >= OCUPACAO_LOTADA_PCT) return { NIVEL_INFO, MOTIVO_LOTADA };
  if (ocupacaoPct <= OCUPACAO_BAIXA_PCT)  return { NIVEL_INFO, MOTIVO_ESTOQUE_BAIXO };
  return { NIVEL_OK, MOTIVO_NENHUM };
}

// O buzzer so toca no que exige alguem ir ate a geladeira agora.
inline bool deveTocarBuzzer(Avaliacao a, bool silenciado) {
  if (silenciado) return false;
  return a.nivel == NIVEL_CRITICO ||
         (a.nivel == NIVEL_ATENCAO && a.motivo == MOTIVO_PORTA_ABERTA);
}

inline const char* nomeNivel(Nivel n) {
  switch (n) {
    case NIVEL_OK:      return "OK";
    case NIVEL_INFO:    return "INFO";
    case NIVEL_ATENCAO: return "ATENCAO";
    case NIVEL_CRITICO: return "CRITICO";
  }
  return "?";
}

// Codigo do motivo (vai no JSON do MQTT).
inline const char* nomeMotivo(Motivo m) {
  switch (m) {
    case MOTIVO_NENHUM:        return "NENHUM";
    case MOTIVO_SENSOR_FALHA:  return "SENSOR_FALHA";
    case MOTIVO_TEMP_ALTA:     return "TEMPERATURA";
    case MOTIVO_PORTA_ABERTA:  return "PORTA_ABERTA";
    case MOTIVO_TEMP_SUBINDO:  return "TEMPERATURA_SUBINDO";
    case MOTIVO_CONGELANDO:    return "CONGELANDO";
    case MOTIVO_UMIDADE_ALTA:  return "UMIDADE";
    case MOTIVO_LOTADA:        return "CAPACIDADE";
    case MOTIVO_ESTOQUE_BAIXO: return "ESTOQUE_BAIXO";
  }
  return "?";
}

// Texto curto (ate 16 caracteres) para a 2a linha do LCD.
inline const char* textoLcd(Motivo m) {
  switch (m) {
    case MOTIVO_NENHUM:        return "Tudo OK";
    case MOTIVO_SENSOR_FALHA:  return "FALHA SENSOR!";
    case MOTIVO_TEMP_ALTA:     return "TEMP ALTA!";
    case MOTIVO_PORTA_ABERTA:  return "FECHE A PORTA";
    case MOTIVO_TEMP_SUBINDO:  return "Temp subindo";
    case MOTIVO_CONGELANDO:    return "Congelando";
    case MOTIVO_UMIDADE_ALTA:  return "Umidade alta";
    case MOTIVO_LOTADA:        return "LOTADA";
    case MOTIVO_ESTOQUE_BAIXO: return "Estoque baixo";
  }
  return "";
}

// ------------------- IoB: comportamento de uso -----------------------
// A geladeira registra COMO a comunidade usa: quantas vezes a porta abre,
// quanto tempo fica aberta e quando a porta e esquecida aberta.
// Com isso os voluntarios descobrem os horarios de pico (melhor hora para
// repor) e o app pode mandar lembretes ("feche a porta").
struct UsoPorta {
  unsigned long aberturas;        // vezes que a porta abriu na janela atual
  unsigned long tempoAbertoMs;    // tempo total aberta na janela
  unsigned long esquecimentos;    // aberturas que passaram de PORTA_ALERTA_MS
};

// msNaJanela: parte do tempo aberta que caiu na janela atual.
// duracaoTotalMs: quanto tempo a porta ficou aberta de verdade.
inline void registrarFechamento(UsoPorta &u, unsigned long msNaJanela, unsigned long duracaoTotalMs) {
  u.tempoAbertoMs += msNaJanela;
  if (duracaoTotalMs >= PORTA_ALERTA_MS) u.esquecimentos++;
}

inline float mediaAberturaSeg(const UsoPorta &u) {
  if (u.aberturas == 0) return 0;
  return (u.tempoAbertoMs / 1000.0f) / u.aberturas;
}

// Classifica o movimento da janela (aberturas por hora).
inline const char* classificarMovimento(unsigned long aberturasPorHora) {
  if (aberturasPorHora >= 20) return "PICO";
  if (aberturasPorHora >= 6)  return "MODERADO";
  if (aberturasPorHora >= 1)  return "BAIXO";
  return "PARADO";
}
