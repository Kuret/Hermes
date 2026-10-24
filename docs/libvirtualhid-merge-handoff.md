# Retomada: migração libvirtualhid antes do merge

Checkpoint de 25/10/2026, Australia/Sydney. Trabalho pausado a pedido do usuário
para reiniciar o computador e continuar em outra máquina. **Não fazer merge na
main do Hermes nem alterar Hermes-KMS nesta retomada sem nova autorização.**

## Continuação de 25/10: descoberta e estado de input

O Hermes agora fixa `faecdd942592e009abed38233e8208cad47e0651` da feature
branch do nosso fork. As referências a `1831bc0` abaixo descrevem o checkpoint
anterior e sua validação, preservados para contexto.

- A biblioteca identifica uinput pelo sysfs do próprio handle. UHID exige o
  phys e o unique ID do controle, inclusive no desktop compartilhado. Isso
  evita misturar controles de mesmo nome/seat, aceitar metadados incompletos
  ou devolver nós de um handle fechado.
- O adapter não reserva slots de touchpad para NaN/inf, confirma alterações
  de ownership somente após envio bem-sucedido, permite release/cancel sem
  coordenadas válidas e evita colisões dos IDs de 7 bits após wrap.
- ENSURE/PURGE/START/STOP agora recusam mutações se não obtiverem um lock
  exclusivo e seguro. O broker rejeita symlink, hard link, FIFO e arquivo
  gravável por outro usuário/grupo, sem bloquear indefinidamente.
- A CI da biblioteca passa a executar nessa feature branch, com GCC/Clang e
  compilação adicional sem XTest. A CI do Hermes compila o broker e inclui
  seus testes de lock junto aos testes de input.

Validação local deste lote: biblioteca e exemplos compilados com GCC 13,
XTest desabilitado e warnings-as-errors; 133 testes da biblioteca aprovados,
incluindo 62 do backend Linux; 5 testes do adapter usando os fontes reais e
runtime fake; 5 testes de lock aprovados; broker compilado com warnings-as-errors.
O arquivo completo de testes do adapter também passou na verificação de sintaxe.
Não foram executados aqui os testes com `/dev/uinput`, `/dev/uhid`, systemd ou
clientes reais. A compilação completa do Hermes e os testes de kernel ficam
para a CI; os resultados anteriores de VM não comprovam este novo lote.

**ACLs automáticas continuam pendentes.** Este lote corrige pré-requisitos de
identidade/serialização; não concede acesso a nenhum dispositivo. A próxima
tarefa autorizada continua sendo o broker registrar conta/UID/seat e conceder,
revalidar e revogar ACLs por dispositivo, inclusive após hotplug/reinício.

## Repositórios e branches

- Hermes: `https://github.com/MrOz59/Hermes.git`, branch
  `experimental/libvirtualhid-input`. Este documento acompanha o commit de
  checkpoint com as mudanças em andamento. A base anterior era `2aa2d4a1`.
- libvirtualhid: `https://github.com/MrOz59/libvirtualhid.git`, branch
  `feat/hermes-isolated-device-discovery`, commit publicado
  `1831bc0d5b43b23ba694d397c8a9d4900186521c`.
- O Hermes já fixa essa revisão da biblioteca em
  `cmake/compile_definitions/linux.cmake`; não depende de override local.
- A master da biblioteca recebeu anteriormente `38904f3` por autorização
  explícita. A mudança nova `1831bc0` está **somente na feature branch**.
- A main do Hermes inspecionada estava em `71c920fc`; era ancestral da feature,
  sem conflitos. Reconsultar os heads remotos antes de uma futura revisão.
- O checkout original desta máquina, `/home/ozzy/Projects/Apollo-Linux`, contém
  mudanças do usuário na branch de Bazzite. Foi preservado. O trabalho desta
  migração está no worktree `/home/ozzy/Projects/Hermes-libvirtualhid` e no clone
  `/home/ozzy/Projects/libvirtualhid-hermes`.

## Bloqueios encontrados na revisão

1. Xbox nativo prefere UHID. Sem acesso do jogo a hidraw, o SDL reconhecia o
   controle, mas perdia rumble básico, mesmo podendo abrir o event node. Isso
   foi reproduzido em VM com UID sem privilégios e importa para o compose, que
   intencionalmente não libera hidraw. Com uinput, rumble funcionava.
2. A CI anterior do Hermes falhava em dois `LinuxRuntimeTest`: o container de
   compilação não possuía `/dev/uinput` e `/dev/uhid`. O Hermes compilava e os
   18 testes do adapter passavam; não era falha de compilação.
3. As contas criadas pelo broker não pertencem a `input`. Os testes de seat
   como root não provam acesso dos jogos sob esses UIDs. O broker ainda não
   concede ACLs aos dispositivos da sessão.

## O que foi implementado neste checkpoint

### Biblioteca: validado e publicado

`CreateGamepadOptions::prefer_input_events` permite escolher uinput para
generic/Xbox mesmo com UHID disponível. Mantém família do controle e rumble
evdev; o perfil efetivo deixa de anunciar trigger rumble e bateria. No Linux,
PlayStation/Switch rejeitam esse pedido, pois não têm esse backend. O padrão
da biblioteca continua inalterado; backends de outras plataformas podem
ignorar a preferência, conforme a documentação da API.

Validação da revisão `1831bc0`:

- Build GCC com warnings-as-errors e exemplos `gamepad_adapter` e
  `keyboard_mouse_adapter` compilados. Os exemplos não foram executados no host.
- Suíte completa em VM: **197 testes, 196 aprovados, 1 skip** por ausência de
  xpadneo. Inclui rumble SDL de Xbox One/Series via uinput com UHID disponível,
  ausência de hidraw e rejeição de perfis incompatíveis.
- [CI GCC 13 e Clang 18 aprovada](https://github.com/MrOz59/libvirtualhid/actions/runs/40131803963).

### Hermes: compilado, testes novos ainda não executados

- Novo `linux_gamepad_backend = auto | native | uinput`, com UI/documentação.
- `auto`: generic/Xbox usam eventos/uinput; PlayStation/Switch usam HID nativo.
- `native`: deixa a biblioteca preferir HID nativo, preservando seus fallbacks.
- `uinput`: seleção automática usa Xbox Series; seleção manual de
  PlayStation/Switch é recusada e a UI avisa sobre o conflito.
- A criação de controles passa `prefer_input_events` à biblioteca.
- Novo consumidor SDL em `tests/consumers/linux_hid_rumble.cpp` e teste
  `XboxEventProfiles/VirtualHidEventKernelTest.*`: processo separado, UID 65534,
  grupos suplementares limpos, acesso ao próprio event node e negação do outro
  seat, com rumble real chegando à fila correta do adapter.
- **Esse teste ainda concede as ACLs manualmente com setfacl dentro da VM. Não
  confundir isso com implementação do broker em produção.**
- Workflow: unitários excluem os dois grupos dependentes de kernel; novo job
  `kernel` recebe os binários, carrega módulos no runner descartável e roda os
  testes reais em Arch/Docker privilegiado. Os smoke tests não foram apenas
  suprimidos: estão previstos no segundo job.
- O job novo e os testes novos do Hermes **ainda precisam ser executados**.
  Conferir particularmente udev/hotplug dentro do container e o guard de VM.

Antes da pausa, o build local de `sunshine`, `test_sunshine` e do consumidor SDL
terminou sem erros. A UI também compilou. Não houve tempo de rodar os testes
desse novo estado antes do pedido de parada. Os resultados anteriores de
**37 testes locais + 25 em VM** correspondem à base `2aa2d4a1`, não comprovam
este checkpoint.

## Próxima tarefa: ACL no broker — autorizada, NÃO implementada

O usuário respondeu explicitamente: **"Sim, implementar ACL por dispositivo e
sessão"**. Pode alterar o broker privilegiado para essa finalidade, sem adicionar
contas ao grupo input nem liberar todos os hidraw. Nenhum arquivo do broker foi
alterado ainda. O trabalho parou na análise do desenho de segurança.

Pontos de integração já localizados:

- `tools/hermes-session-broker.cpp`: serviço root pequeno, serial, protocolo
  por socket, autorização SO_PEERCRED + allow-file root-owned. START recebe
  ambiente; STOP/PURGE encerram contas/unidades. Há lock de arquivo para
  mutações de contas. Novas mutações de concessão/registro/revogação precisam
  de serialização e falha fechada se não puderem adquirir o lock.
- `src_assets/linux/misc/hermes-session-broker.service`: ProtectSystem=strict;
  ReadWritePaths=/etc /var/lib/hermes /run/hermes; capabilities limitadas a
  CHOWN/FOWNER/DAC_OVERRIDE. Não ampliar para grupos ou dispositivos globais.
- `src/process.cpp`: o ambiente do compositor já contém `XDG_SEAT` derivado de
  `runtime.seat_id`. O START inicial é feito via session_broker::start; os apps
  usam scopes ligados à unidade principal.
- `src/platform/linux/session_broker.{h,cpp}`: cliente do protocolo.
- `src/stream.cpp`: input::alloc recebe apenas o seat no caminho isolado.
- `src/platform/linux/input/virtualhid_gamepad.cpp`: após criar um controle,
  descobrir seus nós e pedir a concessão específica ao broker. Considerar
  também os dispositivos de input criados no contexto e os filhos tardios.

Desenho a concluir e testar, não tomar como código existente:

1. Registrar no START a associação cliente/conta/UID/seat validado, sem aceitar
   UID ou caminhos arbitrários vindos do cliente. Recusar colisão entre seats.
2. Conceder ACL apenas a char devices virtuais cujo sysfs comprove o phys/seat
   exato. Para UHID, caminhar até o HID pai e verificar HID_PHYS; para uinput,
   verificar phys. Recusar hardware físico, symlinks, caminhos fora das raízes
   permitidas, travessia e outro seat. Uma ideia era pedir INPUT pelo seat e
   deixar o broker descobrir os nós, sem receber caminhos/UIDs do solicitante.
3. Evitar TOCTOU: abrir com O_NOFOLLOW/CLOEXEC, validar fstat e identidade sysfs;
   alterar ACL pelo fd validado, não reabrir um pathname que pode ser reciclado.
   Se usar setfacl externo com /proc/self/fd, preservar somente esse fd no
   filho antes do exec e manter todos os demais CLOEXEC. Considerar dependência
   de runtime `acl` nos pacotes. Não implementar via shell concatenado.
4. Revogar no STOP, PURGE, falha de launch e reutilização do seat; lidar com
   reinício do broker. Encerrar os processos antes da revogação: ACL não revoga
   descritores já abertos. Não atingir nós de outra sessão ou reaproveitados.
5. Testes de entradas maliciosas e conta/seat errado; teste real com dois UIDs,
   incluindo rumble, hotplug, teardown e negação cruzada. Aproveitar o novo
   consumidor SDL, substituindo a concessão manual pela integração real.

`scripts/session-broker-test.sh` contém um teste end-to-end do broker com contas
e systemd reais. **Só executar em guest descartável**: instala unidades e mexe
nas bases de contas do guest. Há detalhes de virtme-ng/shadow documentados nele.

## Retomar em outra máquina

```bash
git clone --recursive --branch experimental/libvirtualhid-input https://github.com/MrOz59/Hermes.git
cd Hermes
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DSUNSHINE_ENABLE_CUDA=OFF
cmake --build build --target sunshine test_sunshine hermes-session-broker -j4
cd build/tests
./test_sunshine --gtest_filter='VirtualHidInputTest.*:ConfigConsistencyTest.*:LocaleConsistencyTest.*:DirectDevicePort.*:HestiaCapabilitiesSchema.*'
```

Para trabalhar também na biblioteca, clonar recursivamente a feature branch
indicada acima, ler seu AGENTS.md e usar diretórios `cmake-build-*`.

Com udev ativo e root **somente dentro da VM descartável**:

```bash
scripts/vm-isolated-input-test.sh /caminho/absoluto/Hermes/build/tests/test_sunshine
```

Nesta máquina foi usado virtme-ng (`vng`), kernel
`/usr/lib/modules/7.2.9-2-cachyos/vmlinuz`, memória **1G**, 2 CPUs e udevd iniciado
explicitamente. Selecionar um kernel disponível na nova máquina; não copiar
esse caminho cegamente. Os testes básicos usaram vng sem --systemd. O teste
completo do broker exige systemd e ainda não foi rodado nesta etapa.

Ao retomar, conferir a CI disparada pelo push do checkpoint. O objetivo segue
sendo tornar o merge seguro; **não declarar a migração pronta só porque o
build ou a concessão manual de ACL passaram**. Streaming Hestia ponta a ponta
também continua como validação de integração pendente; não há caneta física,
então usar os testes sintéticos.
