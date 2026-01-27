# Implementar filtros digitais no ESP32 usando o ESP-IDF

---

## Quando usar cada um?

### 1. SMA (Simple Moving Average)

O SMA trata todos os pontos da "janela" com o mesmo peso. É como olhar para o espelho retrovisor: ele te dá uma média exata do que aconteceu nos últimos segundos.

* **Utilização:** Ideal para remover ruídos de alta frequência onde você não se importa com um pequeno atraso (lag) na resposta.
* **Exemplo:** Medir a temperatura de um tanque de água. A temperatura não muda bruscamente, então você pode tirar a média dos últimos 10 minutos para ignorar variações momentâneas de leitura.
> **Aplicação:** `Media_Final = (Temp1 + Temp2 + ... + Temp10) / 10`



### 2. EMA (Exponential Moving Average)

O EMA dá mais peso aos dados mais recentes. Ele reage mais rápido a mudanças bruscas do que o SMA, mas ainda assim mantém a suavidade.

* **Utilização:** Ótimo para sistemas que precisam de resposta rápida, mas ainda precisam de estabilidade. Ocupa **menos memória** (não precisa guardar um array/buffer).
* **Exemplo:** Um sensor de nível em um robô equilibrista. Você precisa saber a inclinação *agora*, mas quer filtrar a vibração dos motores.
> **Aplicação:** `Valor_Atualizado = 0.2 * Leitura_Nova + 0.8 * Valor_Antigo`
?