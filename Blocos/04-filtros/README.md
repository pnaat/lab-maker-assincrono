# 📘 Projeto: *[Título do Projeto]* — Aplicação de [CONCEITO] 

---

## Overview
Descrever de forma clara **o que o projeto faz** e **qual conceito será aplicado**.

> **Exemplo:**  
Este projeto demonstra como aplicar **[CONCEITO]** para **[FINALIDADE]** utilizando a **ESP32‑S3 Heltec V3** com **ESP‑IDF**. O foco é implementar [ex.: um filtro EMA para suavizar leituras de temperatura / um ISR para medição precisa de RPM / comunicação SPI com sensor de vibração / MQTT QoS2 para alarmes críticos].

> **Exemplo:**  
The assignment requires the creation of an alarm component, refactoring the original application code to use this new module, and making certain internal thresholds configurable via menuconfig.
The alarm system simulates an event that has a configurable probability of being triggered at a given interval.

---

## Componente

> **Exemplo:**
The `alarm` component exposes the following functions:

```c
alarm_t *alarm_create(void);
bool is_alarm_set(alarm_t *alarm);
void alarm_delete(alarm_t *alarm);
```

* `alarm_create()`: Initializes internal state.
* `is_alarm_set()`: Returns `true` if the alarm is active based on random evaluation and time interval.
* `alarm_delete()`: Frees allocated memory.

---

## Exemplo de Uso

### Example Behavior

The `is_alarm_set()` function checks if a time interval (configurable via `CONFIG_ALARM_REFRESH_INTERVAL_MS`) has passed since the last evaluation. If so, a new random value is generated and compared to `CONFIG_ALARM_THRESHOLD_PERCENT` to determine whether the alarm should be set.

The state is updated only once per interval, reducing redundant random evaluations and ensuring consistent behavior.

### Código-Fonte
```c
// Exemplo EMA
float ema = 0;
ema = alpha * leitura + (1 - alpha) * ema;
```


