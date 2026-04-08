# **TUTORIAL: MONITORAMENTO DE NÍVEL COM PROTOCOLO BLE E CALIBRAÇÃO LINEAR**

## 1. IDENTIFICAÇÃO E AMBIENTE

-   **Trilha:** \[X\] IoT \| IA \| Integração

-   **Nível:** \[X\] Básico \| Intermediário \| Avançado

-   **Ambiente:** Simulação \|\[X\] Kit Físico \| Híbrido

-   **Tempo Estimado:** 90 minutos

## 2. PÚBLICO-ALVO E PRÉ-REQUISITOS

-   **Perfil Recomendado:** Estudantes e entusiastas de sistemas
    embarcados que desejam aprender comunicação sem fio de curto alcance
    e tratamento de dados analógicos.

-   **Conhecimentos Prévios:**

    -   Lógica de programação

    -   Eletrônica básica: leitura de esquemáticos e uso de protoboard.

    -   Navegação básica no ambiente VS Code.

-   **Ferramentas e Softwares:**

    -   VS Code com a extensão ESP-IDF instalada e configurada.

    -   App de scanner BLE (ex: nRF Connect) instalado no smartphone.

## 3. APLICABILIDADE E DIFERENCIAL

-   **O Diferencial (Ferramenta):** O protocolo Bluetooth Low Energy
    (BLE) permite a comunicação direta com dispositivos móveis sem a
    necessidade de uma rede Wi-Fi, reduzindo drasticamente o consumo
    energético e facilitando a configuração local.

-   **A Solução Prática (Projeto):** O uso de um potenciômetro para
    emular uma boia analógica permite testar a lógica de calibração
    necessária para transformar sinais de tensão brutos em informações
    úteis (0-100%). Esta técnica é aplicada no monitoramento de tanques
    de combustível e reservatórios de água.

-   **Habilidade Técnica:** Ao concluir este tutorial, você terá
    dominado: A criação de serviços e características no protocolo BLE e
    a implementação de funções de mapeamento linear para sensores
    analógicos.

-   **Competência Adquirida:** Desenvolver interfaces de sensoriamento
    sem fio para diagnóstico e monitoramento local via dispositivos
    móveis.

## 4. OBJETIVOS DE APRENDIZAGEM

Ao final desta atividade, você será capaz de:

-   **Aplicação Técnica:** Configurar um servidor GATT BLE para
    transmitir dados de telemetria.

-   **Aplicação Funcional:** Desenvolver um sistema de medição de nível
    calibrado que reporta a porcentagem volumétrica de um reservatório.

## 5. FUNDAMENTAÇÃO E CONCEITOS 

-   **Conceito da Ferramenta:** O NimBLE é uma stack Bluetooth
    open-source otimizada para o ESP32. Ela organiza os dados no perfil
    GATT, onde o ESP32 atua como Servidor, disponibilizando uma
    \"Característica\" que o smartphone pode ler ou assinar para receber
    notificações.

-   **Lógica do Projeto:** O projeto utiliza o ADC (Analog-to-Digital
    Converter) do ESP32-S3 para ler o sinal e converter em valores
    digitais. Além disso, utiliza a memória NVS para garantir que a
    média de consumo não seja perdida caso a energia caia.

-   **Lógica do Código:** O código segue o seguinte fluxo:

    -   Inicializa O ESP32 inicia, configura os periféricos (ADC e
        GPIO), o sistema busca na NVS se já existe um consumo_medio_hora
        salvo de sessões anteriores, a stack NimBLE é iniciada e o
        dispositivo começa a \"anunciar\" (Advertising) seu nome para
        que celulares possam encontrá-lo.

    -   Ciclo de Medição: A cada 10 segundos, lê o potenciômetro 10
        vezes e tira a média para evitar oscilações, converte o valor
        para 0-100% e calcula a diferença em relação à última leitura.

    -   Detecção de Vazamento: Se a queda de nível for 20% superior à
        média registrada, o LED de alerta (GPIO 2) é acionado.

    -   Transmissão: O valor é formatado em uma string (ex:
        N:85.0%\|C:0.05\|A:0) e enviado via BLE para qualquer
        dispositivo conectado.

## 6. ACESSO AOS ARQUIVOS DO PROJETO

-   [**Código Base / Firmware:** \[Link para Repositório ou Arquivo
    Fonte\]]{.mark}

-   [**Diagramas e Esquemáticos:** \[Link para Imagem ou PDF em Alta
    Resolução\]]{.mark}

-   **Datasets / Database:** Não se aplica

-   **Arquivos de Hardware:** [\[Link para arquivos de corte laser,
    modelos de impressão 3D (STL) ou indicar \"Não se aplica\"\]]{.mark}

-   **Acesso ao pré-projeto no simulador:** Não se aplica

-   **Lista de Materiais (Kit Físico):**

    -   ESP32‑S3

    -   LED

    -   resistor 1K

    -   Potênciometro/Trimpot de 10K

    -   Jumpers/fios para conexão

    -   Cabo USB para gravação/monitoramento

## 7. ETAPA 1: EXECUÇÃO EM SIMULADOR

-   **Plataforma Sugerida:** Etapa não disponível por limitações
    técnicas do simulador quanto à stack Bluetooth NimBLE e NVS. Avance
    para a Etapa 2

-   **Link de Acesso:** Não se aplica

## 8. ETAPA 2: EXECUÇÃO EM AMBIENTE FÍSICO

-   **Esquema de Montagem para Consulta:** [\[Reinserir Link para
    Imagem/Esquemático\]]{.mark}

**Diretrizes de Rigor Técnico:**

-   **Integridade do Hardware:** O potenciômetro deve ser alimentado por
    3.3V. O uso de 5V na entrada analógica danificará permanentemente o
    GPIO 1 do ESP32-S3.

-   **Qualidade e Organização/Dicas:** : Como trabalhamos com ADC, evite
    fios muito longos para o potenciômetro, pois podem atuar como
    antenas e gerar ruído na leitura do nível.

**Procedimento de Montagem:**

**Passo 01: Montagem da Interface Analógica**

-   **Instrução:** Conecte os pinos laterais do potenciômetro ao 3.3V e
    GND da Heltec. Conecte o pino central (cursor) ao GPIO 1 (ADC1_CH0)

-   **Referência Visual:**

> ![](media/image1.png){width="5.138888888888889in"
> height="3.7847222222222223in"}

*Diagrama de montagem*

-   **Pontos de Atenção:** Certifique-se de que o GND do potenciômetro
    está bem firme para evitar que o valor do nível fique \"saltando\".

**Passo 02: Flash e Monitoramento BLE**

-   **Instrução:**

    1.  Conecte o ESP32‑S3 ao PC via USB e abra o VS Code com o ambiente
        ESP‑IDF configurado.

    2.  SDK Configuration (menuconfig) e certifique-se de que:

        -   Bluetooth esteja habilitado.

        -   Bluetooth host esteja definido como NimBLE.

    3.  Realize o upload do código via VS Code.

    4.  Após o sucesso, abra o app nRF Connect no smartphone, localize o
        dispositivo \"Tanque_Smart_S3\" e conecte-se para ler a
        característica de nível.

-   **Referência Visual:**

![](media/image2.png){width="6.768055555555556in"
height="1.8694444444444445in"}*Configuração Bluetooth*

![](media/image3.png){width="6.768055555555556in"
height="3.0520833333333335in"}

*Validação no terminal*

![Image](media/image4.jpeg){width="3.7083333333333335in"
height="7.827587489063867in"}

*nRF Connect*

-   **Pontos de Atenção:** Se o dispositivo não aparecer, verifique se o
    Bluetooth do seu celular está ativo e se a antena interna do
    ESP32-S3 não está obstruída.

## 9. VALIDAÇÃO E COMPARAÇÃO

-   **Checklist de Sucesso:**

    -   O valor de porcentagem no log serial muda ao girar o
        potenciômetro?

    -   O LED de alerta acende se você girar o potenciômetro rapidamente
        para o lado do GND (simulando vazamento)?

    -   O smartphone consegue ler a string formatada?

**O que muda da simulação para o mundo real?**

+--------------+-----------------------+------------------------------+
| **Variável** | **No Ambiente         | **No Ambiente Real**         |
|              | Controlado**          |                              |
+==============+=======================+==============================+
| **Leitura    | Linearidade perfeita  | Possui ruído térmico; o      |
| ADC**        |                       | código usa filtro de 0.05    |
|              | (0 a 4095)            | para compensar.              |
+--------------+-----------------------+------------------------------+
| **           | Frequentemente        | Mantém o consumo médio mesmo |
| Persistência | reiniciada            | após semanas desligado.      |
| NVS**        |                       |                              |
+--------------+-----------------------+------------------------------+

## 10. RESOLUÇÃO DE PROBLEMAS

  ------------------------------------------------------------------------
  **Se você notar       **A causa pode         **Tente o seguinte\...**
  que\...**             ser\...**              
  --------------------- ---------------------- ---------------------------
  O app não encontra o  Cache do Bluetooth do  Desligue/Ligue o Bluetooth
  BLE                   celular                ou limpe o cache do app nRF
                                               Connect

  Alerta de vazamento   Valor de ultimo_nivel  Verifique se o pino central
  constante             instável               do potenciômetro não está
                                               com mau contato

  Erro de NVS no Boot   Partição NVS           O código já executa
                        corrompida             nvs_flash_erase()
                                               automaticamente se detectar
                                               erro de versão.
  ------------------------------------------------------------------------

## 11. CONCLUSÃO E PRÓXIMOS PASSOS

-   **Glossário:** GATT (Generic Attribute Profile), BLE (Bluetooth Low
    Energy), ADC (Analog-to-Digital Converter), NVS (Non-Volatile
    Storage)

-   **Desafio de Aprimoramento/Expansão:** Altere o INTERVALO_MEDICAO
    para 1 minuto e implemente uma lógica que salve o nível atual na NVS
    a cada 1 hora para evitar perdas de dados em caso de queda de
    energia prolongada.

-   **Referências Adicionais:**

    -   **Documentação NimBLE da Apache**

> https://mynewt.apache.org/latest/network/ble_hs/ble_hs.html

-   **Guia de ADC da Espressif**\
    https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc_oneshot.html

*Este material é um guia complementar para execução independente em
regime assíncrono.*

# Controle de Revisão

  --------------------------------------------------------------------------
  Versão   Descrição            Razão             Autor       Data
  -------- -------------------- ----------------- ----------- --------------
  1.0.0    Tutorial Assincrono  Elaboração        João        05/02/2026
                                Inicial           Santana     

  --------------------------------------------------------------------------
