# Como conectar o ESP32 ao broker em nuvem (HiveMQ Cloud)
 Exemplo de uso do ESP-IDF para conectar o ESP32 ao broker HiveMQ Cloud
 
 O servidor escolhido foi o HiveMQ Cloud. Ele é gratuito e permite a conexão de até 100 dispositivos, além de implementar o broker sobre SSL, sendo necessário usuário, senha e um certificado. Além disso, é possível conectar a ele pelo cliente websocket acessado diretamente pelo browser em http://www.hivemq.com/demos/websocket-client/ .
 
 O certificado SSL está no arquivo .pem no diretório main. Ele deve ser passado como parâmetro na estrutura de configuração do cliente. Se o objeto do cliente não tiver esse certificado, não será possível conectar ao Broker.
