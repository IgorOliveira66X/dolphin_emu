RE4 JPN DROP SEARCH v9.0 - STRUCTURED MOVEMENT POKES
=====================================================

OBJETIVO DESTE PERFIL
---------------------
Encontrar HAND no primeiro drop instantaneo da segunda explosao da vila usando o movie
re4-wr-tas1_explosion2_extended.dtm (8500 frames / 16995 inputs).

A v8 executou 24760 tentativas, incluindo 24416 planos adaptativos distintos, mas todos
chegaram ao mesmo gate $1BC8 e ao mesmo NO_DROP. Isso provou que a busca aleatoria nao estava
cobrindo de maneira sistematica as combinacoes de movimento necessarias.

O QUE MUDOU NA v9
-----------------
- a janela continua depois da animacao: FirstSlot 8428, gate neutro no frame 8465;
- novo input B_ON (a v8 possuia somente B_OFF);
- pokes fortes do analogico principal: 96 e 127;
- duracoes de 1 e 2 slots (2 e 4 frames do movie);
- L, R e B combinados com cada direcao e cada diagonal;
- botao e direcional simultaneos ou separados por um slot;
- dois pokes direcionais separados por 1, 2 ou 3 slots;
- toda a fase deterministica e dividida entre os quatro workers;
- o CSV distingue polls agendados de bytes do controle realmente alterados;
- se um worker terminar sua parte sem mudar o gate $1BC8, ele para em vez de gastar milhares
  de tentativas em fuzz sem feedback.

COMO USAR QUATRO DOLPHINS
-------------------------
1. Use quatro copias separadas desta pasta do Dolphin.
2. Em cada copia, substitua RE4DropSearch.ini pelo arquivo de worker0, worker1, worker2 ou
   worker3 fornecido no pacote de perfis.
3. Confirme no HUD: w0/4, w1/4, w2/4 e w3/4.
4. Inicie obrigatoriamente re4-wr-tas1_explosion2_extended.dtm.
5. Nao use overclock de CPU/VBI emulado. O searcher bloqueia a busca se detectar overclock.

FASES
-----
phase 0: dois replays neutros para validar determinismo e localizar o gate.
phase 1: varredura deterministica particionada de pokes e combinacoes.
phase 2: fuzz adaptativo apenas se o worker tiver encontrado outro gate seed na phase 1.

HUD E CSV
---------
O HUD mostra "input X/Y": X polls mudaram bytes reais do controle; Y polls tinham uma mutacao
agendada. No CSV, os campos scheduled_polls, changed_polls, first_changed_frame,
last_changed_frame e changed_state_hash permitem auditar cada tentativa.

Se todos os workers encerrarem a phase 1 com apenas uma seed, envie os quatro CSVs. Se aparecer
TARGET FOUND, preserve imediatamente o DTM, o TXT, o summary e o CSV daquele worker.
