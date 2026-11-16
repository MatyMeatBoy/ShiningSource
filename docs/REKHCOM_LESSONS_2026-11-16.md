# De ShiningSource a ReKHCoM: Android y online

Fecha: 2026-11-16. Informe técnico de la actualización ShiningSource 0.8.8.

Enviado de vuelta como [ReKHCoM #647](https://github.com/MatyMeatBoy/ReKHCoM/issues/647)
después de publicar la versión y verificar por descarga los cuatro paquetes.

**English summary.** ReKHCoM supplied the relocatable Android image and executable-memory fallback that informed ShiningSource's compatibility work. The strongest lesson to return is SDL3 lifecycle delivery: application lifecycle events reach event watchers without entering the polled queue. Other useful transfers are deterministic identity captured before native pointer patching, adversarial low-memory allocation tests, optional audio initialization, explicit exit diagnostics, and game-aware recovery tests. ReKHCoM already implements acknowledgements, retransmission, adaptive timing, rollback and desync checks; this report does not describe those as missing features.

## Fuentes y alcance

Se consultó por GitHub API el `main` de ReKHCoM y se fijó la comparación en **`6c1951273fc2ccf79369cead1ba207e7297b931d`**, mensaje `docs: add ReKHCoM brand header and badges (#646)`. Las referencias remotas siguientes quedan fijadas a ese commit, no al anterior `f05f0d3…`.

La implementación ShiningSource corresponde al [tag v0.8.8](https://github.com/MatyMeatBoy/ShiningSource/tree/v0.8.8), con [verificación](https://github.com/MatyMeatBoy/ShiningSource/blob/v0.8.8/docs/ANDROID_0.8.8_VERIFICATION.md) y [artefactos y checksums](https://github.com/MatyMeatBoy/ShiningSource/releases/tag/v0.8.8). Las rutas siguientes son relativas a ese árbol; las líneas son orientativas. No se modificó ReKHCoM ni se ejecutaron sus binarios en esta comparación. Una observación del host Android64 no constituye una auditoría de todos sus backends.

## 1. Prioridad alta: observar el lifecycle real de SDL3

En el host Android64 examinado, `sBackground` cambia dentro del `switch` de eventos leídos por polling: [android64_host.c:488](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/port/android64/host/android64_host.c#L488); `H_app_state` lo expone en la línea 546. No aparece un `SDL_AddEventWatch` en ese archivo.

ShiningSource tuvo el mismo supuesto y lo corrigió después de una prueba real: en el SDL3 usado, `SDL_SendAppEvent` notifica directamente a los watchers y **no encola esos eventos para `SDL_PollEvent`**. Tener los casos correctos dentro del polling no prueba que se ejecuten. El cambio está en `runtime/win32/plat_sdl.c:24` y el registro en la línea 311. El watcher recibe entrada a background y `DID_ENTER_FOREGROUND`; el tratamiento restablece input, cola de audio y pacing. No convierte la suspensión de la app en una nueva política de pausa de la partida o de la red.

**Propuesta para ReKHCoM:** comprobar el SDL fijado por el proyecto, instalar el watcher en el camino Android64 si conserva esta semántica y verificar el hilo de entrega antes de hacer trabajo gráfico desde el callback. Reutilizar el tratamiento existente y evitar dobles transiciones. No copiar indiscriminadamente una solución de SDL2 o basarse sólo en eventos sintéticos introducidos con `SDL_PushEvent`.

Otro caso observado en API21 fue `Configuration` con cambios MCC/MNC al
inicializar la SIM: al faltar `mcc|mnc` en `configChanges`, Android destruyó la
actividad SDL y la recreación terminó el proceso. ShiningSource añade esos bits
al juego, manteniendo la recreación normal del launcher. Conviene revisar la
máscara del proyecto para cambios de operador/SIM; no se ha demostrado este
fallo en un binario ReKHCoM. Una prueba debe cambiar configuración real del
sistema y verificar el PID, no sólo llamar un callback artificialmente.

**Prueba de aceptación:** APK con render real, Home, retorno, dedo/botón mantenido y cambio de densidad. Exigir evidencia de ambos estados, ausencia de input/audio acumulado y continuidad del proceso. En ShiningSource, `build/android/088-final2-35-lifecycle/results.json` registra 1200 frames, Android 15, páginas de 16 KiB, ARM64 por native bridge; `run-1.log` contiene watcher instalado, background y foreground. La ejecución incluyó cambio de densidad. Es emulación, no certificación de firmware físico ni de rendimiento a 60 fps.

## 2. Prioridad alta: identidad de contenido independiente del ejecutable

ShiningSource calculaba la identidad online sobre una ROM ya modificada con punteros a funciones nativas. Con la misma ROM, cambiar arquitectura o mover la imagen cambiaba la sala. La corrección captura el fingerprint **antes** de `ss_patch_code_pointers`: `runtime/core/codeptr.c:21` y `:53`; `runtime/core/netlink.c:257` usa esos bytes originales y sube las reglas a **6**. Es una huella de compatibilidad, no autenticación criptográfica.

**Propuesta para ReKHCoM:** convertir esta propiedad en prueba de regresión para cualquier identidad de ROM, build, sala o snapshot. Identificar contenido y reglas con bytes estables, nunca con direcciones parcheadas, padding no inicializado, punteros o estructuras nativas serializadas. Cambiar la versión negociada cuando cambia la semántica incompatible. **No se afirma que ReKHCoM tenga este defecto:** su negociación ya incluye `NET_VERSION 5` y rechaza pares de otra versión, [sio.c:96](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/src/sio/sio.c#L96), [sio.c:262](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/src/sio/sio.c#L262). La recomendación es auditar esa invariante donde corresponda, no reemplazar su handshake por el de otro juego.

**Evidencia ShiningSource:** antes de corregir, los clientes obtenían IDs de sala distintos. Después, huésped Android x86_64 headless, con imagen movida y ventana original ocupada, frente a Windows: 24000 frames por ejecución y **23509 labels compartidos, cero diferencias** en los registros de bloques. Se volvió a comparar `build/android/088-online/android-net.log` con `windows/net.log`: labels 1001–24509. Esto demuestra igualdad del bloque/status registrado por label en esa secuencia; no demuestra por sí solo equivalencia completa de memoria, juego largo ni render/input Android online. No fue una prueba cruzada ARM64/Windows.

## 3. Prioridad media: devolver los casos adversos de memoria, no presentar la técnica como nueva

ReKHCoM **ya tiene** reserva de imagen, relocation Mach-O, W^X y fallback memfd: [ClaimImageSpace:634](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/port/android64/host/android64_host.c#L634), [ExecFromMemfd:677](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/port/android64/host/android64_host.c#L677), [Rebase:691](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/port/android64/host/android64_host.c#L691). ShiningSource aprendió de ese diseño.

Lo transferible de vuelta son las pruebas y algunos bordes: su búsqueda utiliza alineación de 1 MiB (`:637`, `:651`), por lo que conviene probar un hueco suficiente que sólo esté alineado a página; no declarar agotamiento porque una rejilla gruesa no lo encontró. `ExecFromMemfd` hace un único `write` (`:681`): tratar `EINTR` y escrituras cortas evita rechazar un fallback viable. Son mejoras concretas de robustez, no evidencia de un fallo observado en un teléfono ReKHCoM.

ShiningSource separó bounds y fixups en `runtime/host64/image_format.h`, busca huecos reales en `guest_map` (`host_linux.c:130`), valida todo el rango antes de estrechar a 32 bits, copia/reloca en RW y aplica permisos por tramos contiguos. `make_executable` (`:464`) reintenta escrituras y sólo usa `MAP_FIXED` para sustituir páginas ya poseídas. El bridge también separa tabla R y código RX según el tamaño de página. Sólo se mueven slots nativos declarados por el linker; constantes GBA permanecen intactas.

**Casos reutilizables:** ocupar ventanas históricas de imagen/stacks/bridge y primer hint; dejar únicamente un hueco exacto de 68 MiB y otro de 64 KiB por debajo del primer hint; ocupar todo el espacio elegible y exigir fallo limpio; devolver una dirección sobre 4 GiB y comprobar liberación; forzar movimiento y memfd; corromper tablas, destinos, tipos y ULEB. Fixtures: `tools/tests/android_block_stacks.h`, `android_block_image.h`, `test_android_image_format.py`, `android_memory_test.py`. El fallo antiguo se reprodujo; las secuencias headless corregidas alcanzaron 32000 frames y hash `5F1DB1A8`. Los tests de parser son portables; no sustituyen la ejecución nativa ni garantizan todos los casos malformados posibles.

## 4. Prioridad media: audio opcional y diagnósticos que no acusen un reset normal

El host Android64 de ReKHCoM inicializa VIDEO, AUDIO y GAMEPAD conjuntamente: [android64_host.c:893](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/port/android64/host/android64_host.c#L893). Aunque abrir el stream pueda fallar de forma tolerada, un fallo de inicialización del subsistema de audio puede abortar antes. Separar la inicialización de audio permite continuar con vídeo/input si el diseño del juego lo permite. ShiningSource lo hace en `runtime/win32/plat_sdl.c:323`; se probó ARM64 renderizado con movimiento, memfd y driver de audio inexistente durante 600 frames.

ReKHCoM ya tiene instrumentación considerable: [StartupSdlActivity](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/port/android/app/src/main/java/com/rekhcom/android/StartupSdlActivity.java), journals de intentos/proceso y herramientas de lifecycle. Conviene sumar casos de resultado, no reemplazarla por el reporter más pequeño de ShiningSource. En éste se exportan sólo diagnósticos acotados y se diferencian inicio incompleto, fallo explícito y salida normal (`DiagnosticReport.java`). Se detectó y corrigió que el soft reset interno `75` se registraba como error aunque Android salía con `0`: `runtime/host64/host_linux.c:264` ahora normaliza tanto marker como salida. Es un caso de regresión útil si ReKHCoM normaliza códigos internos similares; no se ha demostrado que use ese código.

## 5. Online: comparar recuperación y barreras; conservar lo que ReKHCoM ya hace mejor para su juego

No corresponde una lista de funcionalidades supuestamente ausentes. ReKHCoM ya tiene:

- ACK/retransmisión y revisión de slots: [sio.c:109](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/src/sio/sio.c#L109), `:216`, `:300`.
- Medición RTT, delay/ventana de predicción y rollback: `sio.c:98`, `:121`, `:304`, `:322`; detección de desync sobre estado de batalla en `:376`.
- Transporte Steam/directo/relay y simulación de lag, jitter, pérdida y cortes: [port_netlink.c:4](https://github.com/MatyMeatBoy/ReKHCoM/blob/6c1951273fc2ccf79369cead1ba207e7297b931d/port/src/port_netlink.c#L4). Incluso admite una dirección nueva del peer después de silencio (`:475`).

ShiningSource ofrece otro modelo: bloques etiquetados de 24 bytes, delay adaptativo, ACK/retransmisión (`runtime/core/netlink.c:1`, `:355`), transporte directo separado del canal fiable de mensajes (`:386`), incorporación a partida en un cambio de mapa (`:864`, `:1141`), barreras y estado inicial de incorporación, personaje conservado durante desconexión temporal y reglas explícitas para admitir o excluir el retorno (`:568`, `:1216`), transiciones de ciudad/mapa y votos de salto de escena (`:1616`). `games/ss2/port/patches.json` documenta la integración concreta con esperas, mensajes y generación de enemigos del juego.

**Propuesta:** usar esos escenarios como matriz de fallos para ReKHCoM: pérdida durante una transición/handshake, corte y retorno, peer que cambia de dirección, mensajes repetidos o atrasados, y retorno después de un punto irreversible. Verificar que toda espera termine, que los mensajes de control no queden detrás de estado obsoleto y que el sistema admita/rechace al peer coherentemente. Separar mensajes fiables de estado reemplazable sólo donde el protocolo real lo justifique. No transplantar join-in-progress de un cooperativo de cuatro jugadores a batallas VS de dos, ni eliminar el rollback existente. Que aquí no se haya establecido un equivalente de incorporación en partida no demuestra que el repositorio carezca de él en todos sus modos. El test nuevo de 23509 labels no revalida todos los escenarios históricos de ShiningSource: algunos parches sólo tienen análisis de código y lo indican expresamente.

## 6. Recompilación ARM32: verificar ambos extremos de cada llamada

La prueba estricta cuadro a cuadro encontró un defecto que el hash final y el
audio no detectaban: una declaración alias pasaba `s32 y=-48` a una definición
`u16 y`. ARM AAPCS permite que el callee confíe en la extensión hecha por el
caller; aquí llegaba -48 en vez de 65488 y aparecían ocho sprites ocultos durante
90 cuadros. El backend de 64 bits ya normalizaba estos argumentos. Aplicar la
misma ampliación a palabra y truncado local al ARM32 produjo igualdad en los
32000 cuadros comparados. Se añadió una regresión compilada con el NDK real.

La revisión detectó además que ampliar sólo headers del juego deja incompletos
los límites con reemplazos del runtime que aún reciben `uint16_t`/`int16_t`.
Las copias privadas de fuentes y declaraciones deben normalizarse coherentemente,
incluidos los nombres de tipos `stdint`. También hay dos casos de optimización
que conviene conservar como pruebas: `--wrap` del linker no intercepta llamadas
ya inlineadas dentro del mismo archivo; y un filtro de direcciones bajas copiado
de otro backend puede eliminar accesos GBA válidos antes de traducirlos.

Son lecciones demostradas en ShiningSource, no una afirmación de que ReKHCoM
contenga esos defectos. Propuesta: fixtures pequeños compilados y enlazados con
el toolchain real, más trazas intermedias, no sólo un checksum al final.

## Límite de compatibilidad que sigue abierto

El Galaxy A11 del usuario rechazó la instalación. El APK añade un backend ARMv7 nativo de 32 bits probado en emulación ARM/API21, además de ARM64 y x86_64; **la instalación y ejecución no están verificadas en ese teléfono físico**. Corregir direcciones bajas y relocations en huéspedes ARM64/x86_64 no añade por sí mismo una ABI ausente al APK. Conservar la distinción entre error del instalador, fallo del loader, fallo gráfico/audio y fallo del juego. No deducir la ABI real del sistema instalado únicamente del nombre comercial o del SoC.

Los resultados concretos y limitaciones quedan en el registro de verificación
enlazado. El issue contiene la propuesta para revisión; no se aplicaron cambios
al código de ReKHCoM ni se promete compatibilidad universal.
