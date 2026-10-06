[中文](README.md) | [English](README.en.md) | **Español**

Se ofrece **SR → NR** experimental para Daniel, lmxxf y Mochizuki. Se elige en
**Processing order** del menú Ins; **NR → SR** sigue siendo el valor predeterminado.
El procesamiento posterior usa la resolución de salida y puede consumir más tiempo GPU y VRAM.
Save Settings guarda `[DlssNr] RunBeforeSR=false`; reinicia si el estado solicita los hooks.
Incluye DX12 y los puentes DX11/Vulkan → DX12; no amplía RR ni Vulkan nativos.
Consulta [uso, límites y pruebas](docs/post-sr-nr.md) (en chino).

El backend Mochizuki es compatible con Windows / RDNA4. Consulte [instalación, modelo, controles y validación](docs/mochizuki.md). No se incluyen DLL de NVIDIA ni pesos del modelo. Hay pruebas locales en juegos; otros títulos y escenarios requieren validación.


# OptScaler(NR) 1.10.3

**Agradecimientos especiales**: Gracias a todos los usuarios de Bilibili por sus pruebas y comentarios.

Conecta el **renderizado neuronal de AMD** (DLSS5 on AMD) en **OptiScaler**, permitiendo que juegos **exclusivos de DLSS / XeSS** ejecuten reducción de ruido neuronal (neural denoising) en GPUs AMD; el reescalado sigue a cargo de **FFX/FSR**.

Este proyecto es un fork de **Matheus** y proyectos de la comunidad upstream, manteniendo y evolucionando la base de código con optimizaciones profundas continuas.

**Página del proyecto: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

## Registro de cambios

El registro de cambios detallado está disponible en la [página de Releases](https://github.com/TheAutomatic/dlss-5-amd-project/releases).

---

## Índice
- [1. A hombros de gigantes](#1-a-hombros-de-gigantes)
- [2. Guía de instalación](#2-guía-de-instalación)
  - ├─► [⚡ Instalación rápida](#quick-start)
  - ├─► [Detalles completos de instalación y opciones avanzadas](#detalles-completos-de-instalación-y-opciones-avanzadas)
  - └─► [Opcional: Generación de fotogramas 3x o superior](#opcional-generación-de-fotogramas-3x-o-superior)
- [3. Tres backends y pruebas históricas](#3-tres-backends-y-pruebas-históricas)
- [4. Configuración y controles en el juego](#4-configuración-y-controles-en-el-juego)
- [5. Solución de problemas, registros y desinstalación](#5-solución-de-problemas-registros-y-desinstalación)
- [6. Atribuciones y licencias](#6-atribuciones-y-licencias)

---

## 1. A hombros de gigantes

Este proyecto se basa en los logros colectivos de desarrolladores pioneros en la comunidad de gráficos de código abierto:

| Upstream / Pionero | Su contribución | Lo que añade este proyecto |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | Framework proxy de reescalado universal (DLSS / FFX / XeSS) | Sirve como capa de inyección y host, proporcionando enganches (hooking) y controles de interfaz gráfica |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | Primera integración de DLSS-NR en OptiScaler; diseñaron el pipeline Pre-SR Multi-Pass | Hereda su base de código de OptiScaler y la estructura de despacho Pre-SR |
| **[Matheus / dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project)** | Puente de Pre-SR al runtime de AMD: Entrada DLSS → AMD NR → FFX; integró el mapeo de tonos de dos ramas y OkLab de [RenoDX](https://github.com/clshortfuse/renodx) para mejorar reflejos especulares | Creó la **planificación multi-ranura (Multi-slot)**, eliminando **8.7 ms/fotograma** de bloqueos inactivos de la GPU; adaptó 0.3.1; restauró congelación/restauración de estados D3D12; mejoró compatibilidad con XBOX PC. **Sobrecarga del puente de solo 0.01–0.03 ms** |
| **[danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | Runtime central de AMD Neural Rendering (0.3.0–0.6.0) | Invoca el runtime estándar sin modificaciones centrales; añade protección de estado D3D12 para la espera de dibujado de 1 píxel de 0.3.1+ |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | Red de 71 bloques con ingeniería inversa portada a kernels abiertos AMD HIP | **Integrado en el framework proxy universal OptiScaler para admitir más juegos DLSS / XeSS**; implementó ejecución en la misma cola del fotograma; desarrolló el runtime independiente con C-ABI estandarizado (`LmxxfNrRuntime`); añadió controles deslizantes de ajuste de detalle/color en tiempo real |
| **[Mochizuki / DLSSNR-AMD](https://github.com/mochizuki0323/DLSSNR-AMD)** | Núcleo de red de renderizado neuronal y sombreadores SPIR-V completamente abiertos en Vulkan / RDNA4 | Integración profunda en la arquitectura multi-backend y reestructuración de la seguridad del ciclo de vida: resolución de bloqueos en colas no relacionadas y fugas de VRAM; integración de cancelación de compilación de sombreadores y corrección para controladores 26.9.2+ (proporcionada por [@MatheusFerreiraS](https://github.com/MatheusFerreiraS)) |

Las tarjetas RX 6000 (RDNA2) con danielblnc 0.6.0 requieren AMD HIP 7.2 runtime.

---

## 2. Guía de instalación

### <span id="quick-start"></span>⚡ Instalación rápida (Universal para los 3 backends)

1. **Descargar y descomprimir**: Descargue el archivo `.zip` más reciente desde la [página de Releases](https://github.com/TheAutomatic/dlss-5-amd-project/releases) y extráigalo en cualquier carpeta.
2. **Reunir archivos externos requeridos** (cópielos en la misma carpeta junto a `Setup.bat`, es decir, **1 carpeta + 1 DLL + 1 EXE**):
   - `nvngx_dlssnr.dll` (biblioteca nativa de DLSS-NR de NVIDIA, **actualmente solo se admite la versión 310.8.0**);
   - [Carpeta `native-game-tiled-assets`](https://gofile.io/d/RyvcrDxz) (carpeta de pesos de lmxxf con los archivos del modelo);
   - [`dlssnr_on_amd_setup.exe`](https://github.com/danielblnc/DLSS-NR-on-AMD/releases) (ejecutable de instalación/extracción para danielblnc).
   > 💡 **Consejo**: Si planea utilizar el backend **Mochizuki**, instale previamente **Python 3.10+** desde Microsoft Store.
3. **Ejecutar Setup y seleccionar el juego**:
   - Haga doble clic en `Setup.bat` y seleccione la carpeta donde se encuentra el **ejecutable real del juego** (ej. los juegos de Unreal Engine suelen usar `...\<GameName>\Binaries\Win64\`, no la carpeta del lanzador de la plataforma ni el acceso directo);
   - Siga las instrucciones para elegir el DLL proxy (normalmente `dxgi.dll`; pruebe con `winmm.dll` u otros métodos si no funciona) y el backend. Al actualizar, **se recomienda sobrescribir la instalación existente**.
4. **Nota: No es necesario realizar el paso 2 al actualizar**:
   - Si ya ha instalado previamente este proyecto y los pesos en un juego, **no es necesario realizar el paso 2 al actualizar**; el instalador detectará automáticamente los pesos y archivos existentes en la carpeta del juego y los copiará de vuelta a la carpeta del instalador para su reutilización.

---

### Detalles completos de instalación y opciones avanzadas

<details>
<summary><strong>📖 Haga clic para desplegar: Detalles completos de instalación y opciones avanzadas (Contenido, configuración por backend, actualización/reversión y despliegue manual)</strong></summary>

<details>
<summary><strong>📦 Haga clic para desplegar: Contenido del paquete</strong></summary>

| Archivo / Directorio | Propósito |
|---|---|
| `OptiScaler.dll` | Binario principal (se renombra durante la instalación al nombre de proxy elegido) |
| `OptiScaler.ini` | Archivo de configuración central (contiene opciones de tres backends en `[DlssNr]`) |
| `OptiScaler\` | Dependencias centrales (FFX, XeSS, Agility SDK, plugins) |
| `LmxxfNrRuntime.dll` | Runtime del backend lmxxf (renderizado neuronal HIP de código abierto) |
| `MochizukiNrRuntime.dll` / `dlssnr-amd/shaders/` / `Mochizuki-Model.bat` | Runtime, shaders y herramienta de extracción de Mochizuki; modelo por separado |
| `lmxxf-modules\` | Módulos de cómputo lmxxf de doble arquitectura (38 `.hsaco` para cada una de `gfx1200` / `gfx1201`, con manifiestos `SHA256SUMS`) |
| `shaders\` | Shaders del códec lmxxf (`native_codec_encode.hlsl` y otros) |
| `experimental_lighting\` | Shaders precompilados del pase de iluminación experimental (`GatherCS.cso` / `ResolveCS.cso`) |
| `Setup.bat` / `Setup.ps1` | Instalador interactivo (**Haga doble clic en `Setup.bat`**) |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | Desinstalador seguro (se coloca automáticamente en el directorio del juego) |
| `lmxxf-module-package.ps1` | Asistente de validación de módulos que comparten el instalador y el desinstalador (debe estar junto a `Setup.ps1`) |
| `Licenses\` | Licencias de código abierto de terceros |
| `README.md` / `README.en.md` / `README.es.md` | Documentación (Chino / Inglés / Español) |
| `VERSION` | Versión del paquete |
| `SHA256SUMS.txt` | SHA256 de cada archivo del paquete (verifíquelo con `sha256sum -c SHA256SUMS.txt`) |

> **Nota**: Para cumplir con las licencias y políticas de distribución upstream, este paquete **no incluye** binarios propietarios de NVIDIA, herramientas del instalador de danielblnc ni pesos de modelo no autorizados.

</details>

#### Paso 1: Preparar los archivos del backend (Detallado)

Prepare uno o varios backends:

##### Opción A: [Preparar archivos del backend `lmxxf`](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) o [haga clic aquí](https://gofile.io/d/RyvcrDxz) para descargar los pesos
- `LmxxfNrRuntime.dll` de este paquete completo (no sustituya el runtime ABI1 del upstream);
- Carpeta de módulos `lmxxf-modules\` (estructura de doble arquitectura que incluye subdirectorios `gfx1200` [serie 9060, experimental] y `gfx1201` [serie 9070, producción], con 38 módulos `.hsaco` cada uno y un total de 76 módulos; selección automática según GPU D3D12/HIP; el instalador valida el paquete completo y permite actualizar las instalaciones planas anteriores);
- Carpeta de shaders `shaders\` (con los archivos `.hlsl`);
- Carpeta de pesos `native-game-tiled-assets\` (se puede descargar [aquí](https://gofile.io/d/RyvcrDxz));
- Coloque estos elementos en la misma carpeta descomprimida junto a `Setup.bat`.

Para actualizar, ejecute `Setup.bat` del paquete nuevo y seleccione la carpeta del juego. Si detecta OptiScaler, el instalador recomienda desinstalarlo primero para evitar conflictos entre los archivos nuevos, la estructura de módulos y la configuración anterior. Elija **Y (Recomendado)** para ejecutar automáticamente el desinstalador nuevo y continuar con la instalación, o **N** para sobrescribir la instalación existente. La desinstalación restablece la configuración de OptiScaler y conserva los pesos y las copias de seguridad existentes. La sobrescritura normal no crea copias de seguridad de los DLL, INI ni de las carpetas completas de módulos anteriores. La carpeta anterior se guarda temporalmente para revertir un fallo y se elimina al completar el cambio. Los archivos `.hsaco` añadidos por el usuario y otros contenidos incompatibles con la nueva estructura se guardan aparte en `backup-amd-presr-*/lmxxf-modules`; los demás archivos compatibles del usuario permanecen en su sitio.

##### Opción B: [Preparar archivos del backend `danielblnc`](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)
- `dlssnr_on_amd_setup.exe` y `nvngx_dlssnr.dll` (de los [Releases de danielblnc](https://github.com/danielblnc/DLSS-NR-on-AMD/releases); el instalador genera los pesos automáticamente);
- O los archivos pregenerados `version.dll` y `dlssnr_on_amd_weights.bin`;
- Colóquelos en la misma carpeta descomprimida junto a `Setup.bat`.

---

##### Opción C: Preparar Mochizuki (Windows / RDNA4)

El paquete completo incluye `MochizukiNrRuntime.dll` y `dlssnr-amd/shaders/`. Coloque su propio `nvngx_dlssnr.dll` **310.8.0** (actualmente solo se admite esta versión, SHA256: `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`) junto a `Setup.bat`, instale Python **3.10+** y seleccione Mochizuki en Setup para extraer y validar el modelo. Si ya tiene `dlssnr-amd/dlssnr.bin`, puede reutilizarlo sin extraerlo. Consulte [la instalación de Mochizuki](docs/mochizuki.md) para el hash y la herramienta independiente.

Si falta el DLL de origen o el modelo, Setup muestra `MODEL SETUP REQUIRED`; el runtime solo no basta. No se extraen automáticamente otras versiones del DLL. Mueva el modelo inválido antes de repetir Setup; no se sobrescribe silenciosamente.

La primera compilación puede tardar varios minutos. El panel inferior derecho muestra el progreso mientras se conserva la imagen original. Cambiar resolución, escala o capacidad de pasadas puede exigir otra compilación. Mochizuki tiene preprocesado, tres estilos, 1–3 pasadas (1 por defecto) y controles temporales independientes. Las claves `Mochizuki*` no cambian los otros backends. Para diagnosticar problemas, revise el estado de dependencias/compilación en Ins y después `OptiScaler.log`.

#### Paso 2: Ejecutar el instalador (Detallado)

1. Descomprima este lanzamiento en cualquier carpeta temporal;
2. Coloque los archivos de su backend junto a `Setup.bat`;
3. **Asegúrese de que el juego no se esté ejecutando**;
4. **Haga doble clic en `Setup.bat`**:
   - Seleccione el directorio del ejecutable de su juego (ej. `...\Binaries\Win64\`);
   - Si OptiScaler ya está instalado, elija **Y** para desinstalarlo automáticamente antes de instalar (recomendado), o **N** para sobrescribir;
   - Seleccione el nombre del DLL proxy (predeterminado `dxgi.dll`, recomendado; también se admiten `winmm.dll`, `d3d12.dll`; **no use `dinput8.dll`**; instale previamente los parches correspondientes para los títulos RE Engine de Capcom);
   - Revise los backends y pesos detectados, instale uno o todos los disponibles y elija el backend activo;
   - El instalador configura los proxies, elimina archivos duplicados en conflicto y configura `OptiScaler.ini`.

---

#### Paso 3: Instalación manual

Si prefiere colocar los archivos manualmente:
1. Renombre `OptiScaler.dll` al nombre de proxy elegido (ej. `dxgi.dll`) y cópielo en el directorio del juego;
2. Copie `OptiScaler.ini` y la carpeta `OptiScaler\` en el directorio del juego;
3. **Desplegar archivos de backend**:
   - **Para `lmxxf`**: Copie `LmxxfNrRuntime.dll`, `lmxxf-modules\`, `shaders\` y `native-game-tiled-assets\` en el directorio del juego;
   - **Para `danielblnc`**: Duplique `version.dll` como `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll`; copie `dlssnr_on_amd_weights.bin` en el directorio del juego (**no deje ningún archivo llamado `version.dll`** para evitar doble inyección);
   - **Para `mochizuki`**: Copie `MochizukiNrRuntime.dll`, `dlssnr-amd/shaders/` y su `dlssnr-amd/dlssnr.bin`;
4. En `OptiScaler.ini`, establezca `Enabled = true` en `[DlssNr]` y defina `NrBackend = lmxxf`, `NrBackend = daniel` o `NrBackend = mochizuki`.

</details>

---

### Opcional: Generación de fotogramas 3x o superior

<details>
<summary><strong>👉 Haga clic para desplegar: Generación de fotogramas 3x o superior (Arturs DLSS Enabler / Intel XeFG)</strong></summary>

Estas opciones son independientes de DLSSNR. Los archivos requeridos no están incluidos; obténgalos por separado.
**Nota**: Es necesario reiniciar el juego al modificar los ajustes INI. Mantenga `[FrameGen] External=false`. **No active ambas opciones a la vez**.

---

#### Opción 1: Arturs (DLSS Enabler)
1. Obtenga `dlss-enabler-headless.dll` del autor oficial:
   [Releases de artur-graniszewski/DLSS-Enabler](https://github.com/artur-graniszewski/DLSS-Enabler/releases) o [Nexus Mods 757](https://www.nexusmods.com/site/mods/757)
2. Coloque `dlss-enabler-headless.dll` en la subcarpeta **`OptiScaler\`** dentro del directorio del juego;
3. Si el juego tiene **DLSSG nativo**, configure en `OptiScaler.ini`:
   ```ini
   [FrameGen]
   External=false
   Enabled=true
   FGInput=nvngxfg
   FGOutput=auto
   FGNvngxReplacement=Arturs
   ```
   Si el juego solo tiene reescalado sin DLSSG, use `FGInput=upscaler` + `FGOutput=dlssg`;
4. Compruebe `OptiScaler.log` para confirmar que aparece `Artur's initialized`.

---

#### Opción 2: Intel XeFG (Generación de fotogramas múltiples XeMFG DP4A Unlocker)
1. Coloque `XeFGUnlock.asi` y `XeFGUnlock.ini` en `OptiScaler\plugins\` (junto a `libxess_fg.dll`);
2. Configure `OptiScaler.ini` en la raíz del juego:
   ```ini
   [Plugins]
   LoadAsiPlugins=true

   [FrameGen]
   External=false
   Enabled=true
   FGInput=dlssg
   FGOutput=xefg

   [XeFG]
   InterpolationCount=1
   ```
   - `InterpolationCount`: `1` para 2x, `2` para 3x, etc.;
3. Pruebe primero con 2x antes de aumentar el multiplicador. Presione **Re Pág (Page Up)** para la superposición de FPS y **Av Pág (Page Down)** para estadísticas detalladas.

</details>

---

## 3. Tres backends y pruebas históricas

Los tres backends son lmxxf (HIP), Mochizuki (Vulkan) y Daniel. Las mediciones siguientes son históricas y no representan 1.10.0 ni Mochizuki:

```
                            ┌──► [Backend lmxxf]  ──► HIP abierto / Misma cola del frame / Ajuste profundo
Entradas DLSS/XeSS del juego ──► OptiScaler ──┤
                            ├──► [Mochizuki] ──► Vulkan / D3D12 interop
                            └──► [Backend daniel] ──► Planificación multi-slot / Compat 0.3.1 / Universal
                                        │
                                        ▼
                              Reescalado FFX / FSR ──► Salida final del juego
```

### 1. Backend `danielblnc`: Planificación multi-ranura (Multi-Slot, NR en cada fotograma)

La reducción de ruido (DLSS5) se inserta directamente en la canalización de renderizado: un fotograma debe terminar de desruidarse antes de pasar al reescalado. En configuraciones de ranura única (single-slot), cada fotograma debe esperar a que termine el desruidado del fotograma anterior, causando graves esperas inactivas de la GPU (**MsGPUWait ~8.7 ms/fotograma** en PresentMon). Bajo alta carga, algunos fotogramas se ven obligados a omitir el desruidado, provocando parpadeos o borrosidad visibles.

Este proyecto introdujo la **planificación multi-ranura (Multi-Slot Scheduling)**: asigna búferes paralelos independientes (ranuras) para que cada fotograma pueda avanzar sin esperar la finalización del fotograma anterior en la GPU.

#### Prueba de rendimiento (Carga tipo Onimusha, 4K FSR Ultra Rendimiento = 720p renderizado; comparación con 60 FPS bloqueados)

| Configuración | Tiempo de fotograma medio | FPS aprox. | MsGPUWait (Espera GPU) | Estado NR por fotograma |
|---|---:|---:|---:|---|
| **Single-slot · NR por fotograma (base antigua)** | 29.82 ms | **33.5** | **8.69 ms** | Bloqueado por el fotograma anterior |
| **Nuestro Multi-slot predeterminado** | 22.45 ms | **44.5** (**+33%**) | **≈ 0 ms** | **NR en prácticamente cada fotograma** |
| Upstream 0.3 nativo (referencia) | 22.35 ms | 44.8 | 0 ms | El pipeline nativo no omite fotogramas |

- **Conclusión clave**: Ofrece un aumento de rendimiento del **+33%** (33.5 → 44.5 FPS) optimizando la planificación de la canalización en lugar de comprometer la calidad del denoiser; el cálculo de la red neuronal permanece inalterado (~12–13 ms a 720p).

#### Recomendaciones de ranuras (NR slots: 2–5, predeterminado 3)

| Escena de prueba (4K FSR Ultra Rendimiento, 720p renderizado) | 2 Ranuras | 3 Ranuras |
|---|---:|---:|
| **Onimusha** | 19.50 ms, **0 omitidos** | 19.49 ms, **0 omitidos** |
| **Where Winds Meet** | 19.05–19.25 ms, **Omisiones frecuentes** | 21.78–21.89 ms, **0 omitidos** |

- **Recomendaciones**:
  - **3 ranuras** es el punto óptimo ideal para la mayoría de títulos;
  - Escenas exigentes como *Where Winds Meet* al máximo se benefician de **≥ 3 ranuras**;
  - El costo de VRAM es mínimo: cada ranura es una textura FP16 a resolución de renderizado (~29 MB a 1440p render; ~66 MB en 4K nativo).

### 2. Backend `lmxxf`: Cómputo HIP de código abierto y ejecución en la misma cola del fotograma

- **Soporte de doble arquitectura y autoselección**:
  - **AMD Radeon RX 9070 / 9070 XT (`gfx1201`)**: Arquitectura de producción estándar verificada con 24 módulos optimizados;
  - **AMD Radeon RX 9060 (`gfx1200`)**: Soporte experimental compilado y verificado con COMGR 3.0; pruebas en hardware real y aceleración PDL pendientes;
  - **Selección adaptativa y verificación estricta**: Selección automática de la subcarpeta según D3D12/HIP LUID, con verificación SHA-256 y preflight de símbolos PDL gemelos;
- **Código abierto y optimizado para hardware**: Los módulos de la red neuronal ViT están implementados en HIP, optimizados para arquitecturas RDNA modernas con barreras de grupo de trabajo LDS y modo C32 CU;
- **Ejecución en la misma cola del fotograma**: OptiScaler planifica la grabación de entrada, inferencia HIP y sincronización de barreras en la cola principal antes del cierre de la lista de comandos, eliminando retrasos de sincronización entre procesos;
- **Controles dinámicos de parámetros**: Controles deslizantes continuos en tiempo real para realce de detalle/brillo y calibración de color directamente en el menú Ins.

---

## 4. Configuración y controles en el juego

1. Inicie el juego y entre en el renderizado 3D.
2. Presione **Insert (Ins)** para abrir el menú superpuesto de OptiScaler.
3. Busque la sección **DLSS Neural Rendering** y marque **Enable NR**.
   - La línea de estado indica el runtime activo:
     - `AMD NR runtime: lmxxf` para el backend lmxxf;
     - `AMD NR runtime: 0.3.x` para el backend danielblnc.
4. Pipeline activo: **Entradas DLSS → Reducción de ruido neuronal → Reescalado FFX/FSR**.

### Controles específicos por backend
- **Específicos de `lmxxf`**:
  - `Detail strength`: Control deslizante continuo para detalle y realce de brillo (predeterminado 1.0);
  - `Colour strength`: Control deslizante continuo para saturación y equilibrio de color (predeterminado 1.0);
  - `Debug view`: Visualización en tiempo real de entradas, salida de la red y búferes de diferencias.
- **Específicos de `danielblnc`**:
  - `NR slots`, `Every-frame`, `New wait mode`, `Inline same-frame wait`;
  - `Quality`: Reference (predeterminado, exacto como NVIDIA) / Fast;
  - **Display**: `Tone curve` / `Tone lift`;
  - **Queue (experimental)**: `HIP high-priority queue`;
  - **Compatibility & Scheduling / Diagnostics**: claves extra de `dlssnr_on_amd.ini`.

**Prioridad:** sesión Ins > `OptiScaler.ini` `[DlssNr]` (con Guardar) > `dlssnr_on_amd.ini` / entorno > valores predeterminados.  
Las etiquetas Ins no se escriben al ini; **Guardar** sincroniza ambos ini.  
`OverlayKey` solo controla el overlay propio de daniel.  
Variables de entorno avanzadas (sin interruptor Ins): `DLSSNR_NO_REG`, `DLSSNR_CHAIN`, `DLSSNR_NOBLEND`, `DLSSNR_NO_REPACK`, `DLSSNR_WBLOG`.

**Conmutación en caliente** (menú **Allow backend hot switching**, o `OptiScaler.ini` `[DlssNr]`):

| Clave | Por defecto | Descripción |
|---|---|---|
| `NrConvenience` | `1` | `1`: Prearma los hooks de envío cuando lmxxf o mochizuki están instalados, permitiendo el cambio en caliente en el juego entre los tres backends (daniel ↔ lmxxf ↔ mochizuki); `0`: solo carga el backend seleccionado; cambiar de backend requiere reiniciar el juego. Surte efecto tras reiniciar. |

---

## 5. Solución de problemas, registros y desinstalación

### 1. Desinstalación
1. Abra el **directorio del juego**;
2. Ejecute **`Uninstall_OptiScaler_NR.bat`**;
3. Revise la lista de eliminación propuesta, elija si desea conservar las carpetas de copia de seguridad y confirme con `Y`;
4. **Pesos conservados**: El script está diseñado para conservar los archivos de pesos del usuario (`native-game-tiled-assets/` y `dlssnr_on_amd_weights.bin`) y `nvngx_dlssnr.dll` de forma predeterminada, evitando descargas repetidas de varios gigabytes.

### 2. Ubicación de registros y diagnósticos

Revise los siguientes archivos de registro en el directorio del juego (o en `_storage_` para juegos de Microsoft Store / XBOX PC):
- `OptiScaler.log`: Registro principal de inicialización, enganches y creación de backends;
- `amd_bridge.log`: Registro de la capa de puente de AMD;
- `amd_presr.log`: Registro de despacho Pre-SR;
- `dlssnr_on_amd.log`: Registro del runtime de danielblnc.

> **¿Dónde están los registros de lmxxf y Mochizuki?**  
> A diferencia de `danielblnc` que escribe en un `dlssnr_on_amd.log` independiente, tanto `lmxxf` como `mochizuki` envían todos los mensajes de inicialización, telemetría, compilación de shaders y errores directamente a **`OptiScaler.log`** (y `amd_bridge.log`). No es necesario buscar archivos de registro separados.

#### Diagnósticos del backend `lmxxf`
- **El estado muestra `waiting` o NR no se activa**:
  - Abra `OptiScaler.log` y busque `Lmxxf`;
  - Verifique que `LmxxfNrRuntime.dll` existe en el directorio del juego;
  - Verifique que `lmxxf-modules\` existe y contiene `SHA256SUMS` junto con los módulos de cómputo `.hsaco`;
  - Verifique que `shaders\` existe y contiene los archivos `.hlsl`.
- **Error de pesos no encontrados**:
  - Asegúrese de que el directorio `native-game-tiled-assets\` esté presente en el directorio del juego.
- **Resolución fuera de límites**:
  - Los cortes actuales del modelo lmxxf admiten resoluciones de renderizado **≤ 1080p**. Si juega en 4K, seleccione FSR Rendimiento (renderizado 1080p) o Ultra Rendimiento (renderizado 720p); 4K Calidad (renderizado 1440p) supera los límites de los cortes del modelo.

#### Diagnósticos del backend `danielblnc`
- **El estado no muestra `AMD NR runtime: 0.3.x`**:
  - Asegúrese de que existan `dlssnr_amd_pass1.dll` (y pass2/pass3) y `dlssnr_on_amd_weights.bin`;
  - Asegúrese de que no quede ningún `version.dll` en conflicto en el directorio del juego;
  - Compruebe `dlssnr_on_amd.log` para ver si hay errores de inicialización del runtime.

#### Diagnósticos del backend `mochizuki`
- **El estado indica dependencias faltantes o NR no se activa**:
  - Abra `OptiScaler.log` y busque `mochizuki`, o revise el estado de dependencias faltantes en el menú Ins;
  - Verifique que `MochizukiNrRuntime.dll` existe en el directorio del juego;
  - Verifique que `dlssnr-amd/shaders/` y `dlssnr-amd/dlssnr.bin` estén presentes y sean válidos;
  - La primera compilación de la red puede tardar varios minutos; el progreso se muestra en el panel inferior derecho y se conserva la imagen original hasta que finalice.
- **Comprobaciones del entorno**:
  - Asegúrese de tener instalado un controlador AMD compatible con Vulkan; verifique que Python 3.10+ de Microsoft Store estuviera presente durante la extracción inicial del modelo.

#### Notas sobre Microsoft Store / XBOX PC
Debido a la virtualización del sistema de archivos de Windows, ciertos títulos de Store / Game Pass crean una carpeta **`_storage_`** junto al ejecutable. Revise esta carpeta si los registros o salidas no aparecen en el directorio principal del juego.

### 3. Formato para reportar problemas
Al reportar problemas, por favor incluya:
1. Nombre del DLL proxy utilizado (ej. `dxgi.dll`);
2. Backend seleccionado (`lmxxf`, `daniel` o `mochizuki`);
3. Modelo de GPU, versión del sistema operativo y versión del controlador AMD;
4. Título del juego, resolución de salida y modo FSR;
5. Archivos `.log` relevantes indicados anteriormente.

---

## 6. Atribuciones y licencias

Patrimonio del código base (de arriba a abajo):  
[OptiScaler](https://github.com/optiscaler/OptiScaler) → [Dagherbou](https://github.com/Dagherbou/OptiScaler_DLSSNR) → [wilsjo2](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) → [Matheus](https://github.com/MatheusGViana/dlss-5-amd-project) → [**Este repositorio (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project).

- [**OptiScaler**](https://github.com/optiscaler/OptiScaler) — **Licencia GPL-3.0**: Framework proxy de reescalado universal;
- [**Dagherbou / OptiScaler_DLSSNR**](https://github.com/Dagherbou/OptiScaler_DLSSNR) — **Licencia GPL-3.0**: Integración inicial de DLSS-NR;
- [**wilsjo2 / OptiScaler-DLSSNR-PreSR-Multipass**](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) — **Licencia GPL-3.0**: Arquitectura Pre-SR y Multi-Pass;
- [**Matheus / dlss-5-amd-project**](https://github.com/MatheusGViana/dlss-5-amd-project) — **Licencia GPL-3.0**: Puente AMD Pre-SR;
- [**danielblnc / DLSS-NR-on-AMD**](https://github.com/danielblnc/DLSS-NR-on-AMD) — **Licencia personalizada no comercial / Todos los derechos reservados**: El autor retiene todos los derechos; redistribución prohibida; integrado mediante detección externa;
- [**lmxxf / dlss5-on-amd-9070xt-porting**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) — **Licencia MIT**: Núcleo de renderizado neuronal HIP de código abierto y recuperación de red de 71 bloques;
- [**Mochizuki / DLSSNR-AMD**](https://github.com/mochizuki0323/DLSSNR-AMD) — **Licencia MIT**: Núcleo de red de renderizado neuronal completamente abierto en Vulkan / RDNA4 y sombreadores SPIR-V;
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) — **Licencia MIT**: Algoritmos de composición de color en `dlssnr.hlsl`;
- [**Este proyecto (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project) — **Licencia GPL-3.0**: Planificación multi-ranura, ejecución en la misma cola del fotograma, creación de runtime con C-ABI y PR upstream, congelación/restauración de estado 0.3.1, coexistencia de doble backend e instalador inteligente.

Esta distribución no contiene binarios propietarios de NVIDIA, herramientas del instalador de danielblnc ni pesos de modelo no autorizados. Por favor, respete todas las licencias upstream.
