# Launcher WebUI - Dev Server

Backend Node.js que replica o comportamento do `src/webInterface.cpp` para testar a interface web sem precisar de hardware ESP32.

## Requisitos

- Node.js 16+
- Sem dependencias externas

## Uso

```bash
node server.js <pasta-raiz>
```

A `<pasta-raiz>` e tratada como o cartao SD. Todos os arquivos listados, baixados, enviados e deletados operam dentro dessa pasta.

Exemplos:

```bash
node server.js C:\Users\bmorc\Downloads
node server.js D:\testes\sdcard
```

Via npm (de dentro da pasta `backend/`):

```bash
npm start -- C:\Users\bmorc\Downloads
```

Porta e credenciais via variaveis de ambiente:

```bash
PORT=3000 WUI_USR=admin WUI_PWD=minhasenha node server.js C:\pasta
```

Apos iniciar, abra `http://localhost:8080`.

Login padrao: `admin` / `admin`

## Endpoints implementados

| Endpoint | Metodo | Comportamento |
|---|---|---|
| `/ping` | GET | Retorna `launcher-pong` |
| `/login` | POST | Autentica e cria cookie de sessao |
| `/logout` | GET | Encerra sessao e redireciona |
| `/systeminfo` | GET | Versao mock + estatisticas do SD |
| `/listfiles?folder=` | GET | Lista real do diretorio |
| `/file?name=&action=` | GET | Download, delete ou create (pasta) |
| `/editfile?name=` | GET / POST | Le e salva arquivos de texto |
| `/` | POST | Upload de arquivos (multipart) |
| `/rename` | POST | Renomeia arquivo ou pasta |
| `/nvs` | GET / POST | Dados NVS persistidos em `nvs_mock.json` |
| `/wifi` | GET | Simulado |
| `/sdpins` | GET | Simulado |
| `/reboot` | GET | Simulado |
| `/OTA?update=1` | GET | Entra em modo de update e limpa o contexto OTA simulado |
| `/OTA` | POST | Valida `command`, `size` e `manifest`, preparando a instalacao em serie |
| `/OTAFILE` | POST | Recebe o binario completo e simula a gravacao sequencial dos blocos do manifest |
| `/UPDATE` | POST | Simulado |

## OTA simulado

O backend mock acompanha o fluxo novo do firmware:

1. `GET /OTA?update=1`
2. `POST /OTA` com `command=0`, `size` e `manifest`
3. `POST /OTAFILE` com o binario completo

Quando existe `manifest`, o servidor:

- valida os ranges de cada part
- exige exatamente uma part do tipo `app`
- ordena as parts por `sourceOffset`
- simula a gravacao sequencial do mesmo arquivo enviado no upload

Sem `manifest`, ele mantem apenas um fallback legado simulado.

## NVS

Os dados NVS ficam em `nvs_mock.json` na mesma pasta do servidor e persistem entre reinicializacoes. Na primeira execucao, um conjunto de chaves de exemplo e criado automaticamente.

A chave `launcher/token` nunca e exposta nem editavel, assim como no firmware.

## Seguranca

Todas as operacoes de arquivo ficam restritas a pasta raiz informada na inicializacao. Tentativas de path traversal sao bloqueadas.
