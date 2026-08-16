RE4 JPN DROP SEARCH v8.0 - VILLAGE ROUTE SCOUT
================================================

OBJETIVO DESTA VERSAO
---------------------
A v7.2 registrou 51.519 tentativas validas nos quatro workers, mas todas as rotas de granada
observadas chegaram ao seed $BE9D e ao grenade ID 2. As rotas de dinheiro produziram apenas
700..1400 ptas e nunca passaram do primeiro money probe. Isso indica um plato estrutural: repetir
o mesmo espaco de busca por mais tempo provavelmente so visitaria variacoes do mesmo ramo.

A v8.0 e primeiro um scout de cobertura. Ela ainda reconhece o alvo final Hand Grenade + 9900
ptas, mas tambem pausa e grava o DTM assim que encontra qualquer um destes breakthroughs:

  - route seed diferente de $BE9D;
  - grenade ID diferente de 2 (Hand e ID 1);
  - segundo ou terceiro money probe alcancado;
  - valor de ptas fora de 700, 800, 900, 1000, 1100, 1200, 1300 e 1400.

Um SCOUT BREAKTHROUGH nao significa necessariamente que o alvo final ja apareceu. Ele significa
que a busca finalmente escapou do ramo repetitivo da v7.2. O DTM gerado permite confirmar esse
ramo no jogo e usar seus resultados para ajustar a busca final sem recomecar no escuro.

MOVIE E JANELA VALIDADA
-----------------------
O RE4DropSearch.ini incluido foi preparado exclusivamente para o novo movie enviado:

  re4-wr-tas1(1).dtm
  Game ID: G4BJ08
  Frames: 8106
  Inputs: 16207

No movie, os dois polls permanecem B + analogico (57,199) entre os frames 7747 e 7801, durante
a animacao da granada. A transicao para a corrida normal ocorre no frame 7802. Por isso:

  - snapshot: frame 7803;
  - primeira manipulacao: slot 7806, que altera os frames/polls 7805 e 7806;
  - limite de perda de movimento: 4 frames emulados;
  - duas mortes/drops simultaneos;
  - alvo final: HAND + PTAS:9900, em qualquer ordem;
  - budget padrao: 20.000 tentativas por worker.

COMO USAR COM UM DOLPHIN
------------------------
1. Extraia todo o ZIP para uma pasta nova.
2. Mantenha RE4DropSearch.ini ao lado de Dolphin.exe.
3. Abra o movie re4-wr-tas1(1).dtm em modo read-only, com o mesmo setup do RE4 JPN.
4. Aguarde a validacao e a calibracao completa. Depois o HUD entra na phase 2.
5. Nao habilite CPU Clock Override nem VBI Overclock emulado. A ferramenta recusa iniciar se
   detectar qualquer um deles, pois mudariam o timing do TAS.

COMO USAR COM QUATRO DOLPHINS
-----------------------------
Use quatro copias da pasta portatil, cada uma com seu proprio User. Em todos os INIs:

  WorkerCount = 4
  PartitionWorkers = True

Depois configure um WorkerId diferente em cada copia:

  Dolphin 1: WorkerId = 0
  Dolphin 2: WorkerId = 1
  Dolphin 3: WorkerId = 2
  Dolphin 4: WorkerId = 3

A validacao e a calibracao inicial sao repetidas em cada Dolphin de proposito, pois cada worker
precisa medir localmente a janela. Na phase 2, cada plano recebe um hash estavel e pertence a
exatamente um WorkerId. Portanto, workers com o mesmo WorkerCount nao testam o mesmo plano exato,
mesmo que seus algoritmos adaptativos tentem gera-lo por caminhos diferentes.

O QUE ACONTECE QUANDO ACHAR
---------------------------
O Dolphin que encontrar algo pausa e mostra uma destas mensagens:

  RE4 v8.0 TARGET FOUND
  RE4 v8.0 SCOUT BREAKTHROUGH

Os outros Dolphins nao param automaticamente. Pause-os manualmente e preserve, no minimo, os
arquivos do worker vencedor:

  re4_jpn_village_dual_v8_scout_wN_FOUND.dtm
  re4_jpn_village_dual_v8_scout_wN_FOUND.txt
  re4_jpn_village_dual_v8_scout_wN_summary.txt
  re4_jpn_village_dual_v8_scout_wN_results.csv

O FOUND.txt informa a razao exata, por exemplo ROUTE_BE9B, GRENADE_ID_1, MONEY_PROBE_2 ou
PTAS_3300. O FOUND.dtm ja contem todos os polls vencedores, inclusive subframes que nao sao
praticos de editar manualmente no Dolphin.

Se o budget terminar sem breakthrough, envie os quatro results.csv e summary.txt. A coluna
coverage_score e os campos novel_* mostram quais familias de resultados a nova janela alcancou.

ARQUIVOS DE SAIDA
-----------------
O HUD exibe o caminho exato do CSV. O local primario e User/Logs; se ele nao for gravavel, a
ferramenta usa RE4DropSearchResults ao lado de Dolphin.exe. Um arquivo
RE4DropSearchOutput_wN.txt ao lado do executavel tambem aponta para o CSV ativo.

PERFIL GENERICO
---------------
O motor continua configuravel pelo INI para 1..10 drops e outros alvos. Para uma busca final
normal, use Scout Enabled=False. Para mapear outro plato, mantenha Scout Enabled=True e atualize
KnownRouteSeeds, KnownGrenadeIds, KnownMoneyAmounts e MinimumMoneyProbeStage com os resultados
ja conhecidos. Alterar filme, janela, alvo, numero de drops ou catalogo nao exige recompilacao;
portar os probes para outro executavel/regiao do jogo ainda exige novos enderecos.
