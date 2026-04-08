# TUTORIAL: MONITORAMENTO DE NÍVEL COM PROTOCOLO BLE E CALIBRAÇÃO LINEAR

## 1. Visão Geral do Projeto

Este tutorial guia você pela criação de um sistema de monitoramento de nível utilizando o protocolo Bluetooth Low Energy (BLE) e calibração linear em um ESP32-S3. O projeto demonstra como transformar leituras analógicas de um sensor (emulado por um potenciômetro) em informações úteis, como a porcentagem de nível de um reservatório, e transmiti-las para um dispositivo móvel.

**Trilha:** IoT

**Nível:** Básico

**Ambiente:** Kit Físico

**Tempo Estimado:** 90 minutos

## 2. Aplicabilidade e Diferenciais

### O Diferencial (Ferramenta)

O protocolo Bluetooth Low Energy (BLE) permite a comunicação direta com dispositivos móveis sem a necessidade de uma rede Wi-Fi, reduzindo drasticamente o consumo energético e facilitando a configuração local.

### A Solução Prática (Projeto)

O uso de um potenciômetro para emular uma boia analógica permite testar a lógica de calibração necessária para transformar sinais de tensão brutos em informações úteis (0-100%). Esta técnica é aplicada no monitoramento de tanques de combustível e reservatórios de água.

### Habilidades e Competências Adquiridas

Ao concluir este tutorial, você terá dominado a criação de serviços e características no protocolo BLE, a implementação de funções de mapeamento linear para sensores analógicos e será capaz de desenvolver interfaces de sensoriamento sem fio para diagnóstico e monitoramento local via dispositivos móveis.

## 3. Objetivos de Aprendizagem

Ao final desta atividade, você será capaz de:

- **Aplicação Técnica:** Configurar um servidor GATT BLE para transmitir dados de telemetria.
- **Aplicação Funcional:** Desenvolver um sistema de medição de nível calibrado que reporta a porcentagem volumétrica de um reservatório.

## 4. Fundamentação e Conceitos

### Conceito da Ferramenta (NimBLE)

O NimBLE é uma stack Bluetooth open-source otimizada para o ESP32. Ele organiza os dados no perfil GATT, onde o ESP32 atua como Servidor, disponibilizando uma "Característica" que o smartphone pode ler ou assinar para receber notificações.

### Lógica do Projeto

O projeto utiliza o ADC (Analog-to-Digital Converter) do ESP32-S3 para ler o sinal e converter em valores digitais. Além disso, utiliza a memória NVS para garantir que a média de consumo não seja perdida caso a energia caia.

### Lógica do Código

O código segue o seguinte fluxo:

1. **Inicialização:** O ESP32 inicia, configura os periféricos (ADC e GPIO), busca na NVS se já existe um `consumo_medio_hora` salvo de sessões anteriores, a stack NimBLE é iniciada e o dispositivo começa a "anunciar" (Advertising) seu nome ("Tanque_Smart_S3") para que celulares possam encontrá-lo.
2. **Ciclo de Medição:** A cada 10 segundos, lê o potenciômetro 10 vezes e tira a média para evitar oscilações, converte o valor para 0-100% e calcula a diferença em relação à última leitura.
3. **Detecção de Vazamento:** Se a queda de nível for 20% superior à média registrada, o LED de alerta (GPIO 2) é acionado.
4. **Transmissão:** O valor é formatado em uma string (ex: `N:85.0%|C:0.05|A:0`) e enviado via BLE para qualquer dispositivo conectado.

## 5. Pré-requisitos

### Perfil Recomendado

Estudantes e entusiastas de sistemas embarcados que desejam aprender comunicação sem fio de curto alcance e tratamento de dados analógicos.

### Conhecimentos Prévios

- Lógica de programação
- Eletrônica básica: leitura de esquemáticos e uso de protoboard.
- Navegação básica no ambiente VS Code.

### Ferramentas e Softwares

- VS Code com a extensão ESP-IDF instalada e configurada.
- App de scanner BLE (ex: nRF Connect) instalado no smartphone.

## 6. Materiais Necessários (Kit Físico)

- ESP32-S3
- LED
- Resistor 1K
- Potenciômetro/Trimpot de 10K
- Jumpers/fios para conexão
- Cabo USB para gravação/monitoramento

## 7. Diagrama de Conexão

**(Referência:** `schematic.png` no diretório raiz do projeto ou visualizar o diagrama abaixo)

**Instruções de Montagem:**

- **Potenciômetro:** Conecte os pinos laterais do potenciômetro ao 3.3V e GND da Heltec. Conecte o pino central (cursor) ao GPIO 1 (ADC1_CH0) do ESP32-S3.
- **LED:** O tutorial não especifica a conexão do LED, mas geralmente é conectado a um pino GPIO via um resistor de 1K para limitar a corrente.
- **Pontos de Atenção:**
  - O potenciômetro deve ser alimentado por 3.3V. **O uso de 5V na entrada analógica danificará permanentemente o GPIO 1 do ESP32-S3.**
  - Evite fios muito longos para o potenciômetro, pois podem atuar como antenas e gerar ruído na leitura do nível.
  - Certifique-se de que o GND do potenciômetro está bem firme para evitar que o valor do nível fique "saltando".

## 8. Guia de Configuração e Uso

### 8.1. Flash e Monitoramento BLE

1. **Conecte o ESP32-S3 ao PC via USB** e abra o VS Code com o ambiente ESP-IDF configurado.
2. **SDK Configuration (menuconfig):** Certifique-se de que o Bluetooth esteja habilitado e o Bluetooth host esteja definido como NimBLE.
3. **Realize o upload do código** via VS Code.
4. Após o sucesso, abra o app nRF Connect no smartphone, localize o dispositivo "Tanque_Smart_S3" e conecte-se para ler a característica de nível.

### 8.2. Validação e Comparação

- **Checklist de Sucesso:**
  - O valor de porcentagem no log serial muda ao girar o potenciômetro?
  - O LED de alerta acende se você girar o potenciômetro rapidamente para o lado do GND (simulando vazamento)?
  - O smartphone consegue ler a string formatada (`N:XX.X%|C:X.XX|A:X`)?

- **O que muda da simulação para o mundo real?**

  | Variável | No Ambiente Controlado | No Ambiente Real |
  | :------------------ | :------------------------ | :-------------------------------------------------- |
  | **Leitura ADC** | Linearidade perfeita | Possui ruído térmico; o código usa filtro de 0.05. |
  | **Persistência NVS**| Frequentemente reiniciada | Mantém o consumo médio mesmo após semanas desligado.|

## 9. Resolução de Problemas Comuns

| Se você notar que...          | A causa pode ser...              | Tente o seguinte...                                                                 |
| :---------------------------- | :------------------------------- | :---------------------------------------------------------------------------------- |
| O app não encontra o BLE      | Cache do Bluetooth do celular    | Desligue/Ligue o Bluetooth ou limpe o cache do app nRF Connect.                     |
| Alerta de vazamento constante | Valor de `ultimo_nivel` instável | Verifique se o pino central do potenciômetro não está com mau contato.              |
| Erro de NVS no Boot           | Partição NVS corrompida          | O código já executa `nvs_flash_erase()` automaticamente se detectar erro de versão. |

## 10. Próximos Passos e Aprimoramentos

- **Desafio de Aprimoramento/Expansão:** Altere o `INTERVALO_MEDICAO` para 1 minuto e implemente uma lógica que salve o nível atual na NVS a cada 1 hora para evitar perdas de dados em caso de queda de energia prolongada.

## 11. Referências Adicionais

- **Documentação NimBLE da Apache:** [https://mynewt.apache.org/latest/network/ble_hs/ble_hs.html](https://mynewt.apache.org/latest/network/ble_hs/ble_hs.html)
- **Guia de ADC da Espressif:** [https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc_oneshot.html](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc_oneshot.html)

## 12. Glossário

- **GATT:** Generic Attribute Profile
- **BLE:** Bluetooth Low Energy
- **ADC:** Analog-to-Digital Converter
- **NVS:** Non-Volatile Storage

---

_Este material é um guia complementar para execução independente em regime assíncrono._
