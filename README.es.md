[中文](README.md) | [English](README.en.md) | **Español**

# OptiScaler AMD pre-SR — 1.9.0.3

Conecta el **renderizado neuronal de AMD** (DLSS5 on AMD) en **OptiScaler**, permitiendo que juegos **exclusivos de DLSS / XeSS** ejecuten reducción de ruido neuronal (neural denoising) en GPUs AMD; el reescalado sigue a cargo de **FFX/FSR**.

Este proyecto es un fork de **Matheus** y proyectos de la comunidad upstream, manteniendo y evolucionando la base de código con optimizaciones profundas continuas.

**Página del proyecto: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## Índice
- [📢 Registro de cambios 1.9.0 (Changelog)](#-registro-de-cambios-190-changelog)
- [1. A hombros de gigantes](#1-a-hombros-de-gigantes)
- [2. Guía de instalación](#2-guía-de-instalación)
  - └─► [Opcional: Generación de fotogramas 3x o superior](#opcional-generación-de-fotogramas-3x-o-superior)
- [3. Arquitectura de doble backend y pruebas de rendimiento](#3-arquitectura-de-doble-backend-y-pruebas-de-rendimiento)
- [4. Configuración y controles en el juego](#4-configuración-y-controles-en-el-juego)
- [5. Solución de problemas, registros y desinstalación](#5-solución-de-problemas-registros-y-desinstalación)
- [6. Atribuciones y licencias](#6-atribuciones-y-licencias)

---

## 📢 Registro de cambios 1.9.0 (Changelog)

La versión 1.9.0 es una **actualización arquitectónica fundamental**. Introducimos oficialmente el backend de renderizado neuronal de código abierto [**`lmxxf` HIP**](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) y resolvemos dificultades críticas de compatibilidad de múltiples colas y división de listas de comandos en títulos modernos con Unreal Engine 5.

### 🚀 Aspectos destacados

1. **Correcciones de compatibilidad con Unreal Engine 5 (UE5) para lmxxf (*Neverness to Everness*, *Palworld*, etc.) (1.9.0.3)**
   - **Vinculación a la cola de renderizado (*Neverness to Everness*)**: Se vincula correctamente a la cola Direct de renderizado real del juego que ejecuta los comandos de DLSS-NR, evitando bloqueos e invalidación de sesión causados por discrepancias entre la cola de renderizado del viewport y la cola de presentación de la Swapchain (`QueueContract: targetQueue != sessionQueue`).
   - **Protección de seguridad de cola**: Añade comprobaciones de identidad COM durante los callbacks de ejecución de listas de comandos para omitir la evaluación de HIP de forma segura cuando se despachan listas de comandos inesperadas.
   - **Vaciado de GPU en migración**: Vacía la GPU (drain) antes de la migración de colas para reducir los riesgos de fugas de VRAM por recreación de sesiones.
   - **División de listas de comandos y correcciones de inicio (*Palworld*)**: Refuerza las comprobaciones de admisibilidad de división, ajusta el nivel de registro predeterminado a 2 (Information) para eliminar los retrasos de hash de inicio y aplica limitación de tasa (rate-limiting) en los registros.

2. **Nuevo backend de renderizado neuronal `lmxxf`**
   - **Núcleo de cómputo de código abierto**: Además de mantener la compatibilidad con el backend existente de `danielblnc`, se integra el núcleo de renderizado neuronal HIP de código abierto.
   - **Ejecución en la misma cola del fotograma (Same-Frame Queue Execution)**: Integra la grabación de entrada, inferencia asíncrona HIP y sincronización de barreras dentro de la cola de comandos principal del juego antes del reescalado (Pre-SR).
   - **Soporte de proxy DLSS / XeSS para lmxxf**: Permite que el backend `lmxxf` intercepte entradas DLSS y XeSS antes del reescalado (Pre-SR), permitiendo que juegos sin FSR nativo utilicen el denoiser de lmxxf.
   - **Compatibilidad con doble backend**: Soporte perfecto para los backends `lmxxf` y `danielblnc`. Cambie entre ellos en cualquier momento en `OptiScaler.ini` mediante `NrBackend=lmxxf` o `NrBackend=daniel`.
   - **Optimización de memoria y estabilidad**: Optimiza el modo `fast_prefix` para omitir la asignación redundante del buffer de ruido de 201 MB, reduciendo el consumo de memoria del host y la sobrecarga de inicio. Mejora la coincidencia de LUID de GPU en Fake NVAPI para evitar bloqueos entre adaptadores en entornos multi-GPU o simulados.
   - **⚠️ Recomendación de resolución**: La arquitectura del modelo actual de `lmxxf` está optimizada para **resolución de renderizado previa al reescalado ≤ 1080p**:
     - **Salida 4K**: Se recomienda usar **FSR Rendimiento** (renderizado a 1080p) o Ultra Rendimiento (renderizado a 720p).
     - **Salida 1440p (2K)**: Se recomienda usar **FSR Calidad / Equilibrado / Rendimiento** (todos renderizan a 1080p o inferior).
     - **Salida 1080p**: Admite **1080p Nativo** o cualquier modo de escalado FSR.

3. **Actualización del instalador**
   - Se corrigió la lógica de interacción del instalador, admitiendo selección de doble backend y coexistencia/sobrescritura segura.

4. **Mejoras en el menú (Menú Ins) y controles deslizantes en tiempo real**
   - **Menú contextual inteligente**: Oculta automáticamente las opciones específicas de Daniel (ej. slots, passes, new wait) en modo `lmxxf` para evitar confusiones.
   - **Corrección de diseño**: Resuelve la superposición entre `Enable NR` y `AMD processing`, restaurando una estructura vertical clara.
   - **Controles deslizantes en vivo**: Introduce controles deslizantes continuos para `Detail strength` y `Colour strength`, junto con un selector de canales de `Debug view` en tiempo real para diagnóstico visual en directo.

---

## 1. A hombros de gigantes

Este proyecto se basa en los logros colectivos de desarrolladores pioneros en la comunidad de gráficos de código abierto:

| Upstream / Pionero | Su contribución | Lo que añade este proyecto |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)** | Framework proxy de reescalado universal (DLSS / FFX / XeSS) | Sirve como capa de inyección y host, proporcionando enganches (hooking) y controles de interfaz gráfica |
| **[Dagherbou / OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)** → **[wilsjo2 / PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)** | Primera integración de DLSS-NR en OptiScaler; diseñaron el pipeline Pre-SR Multi-Pass | Hereda su base de código de OptiScaler y la estructura de despacho Pre-SR |
| **[Matheus / dlss-5-amd-project](https://github.com/MatheusGViana/dlss-5-amd-project)** | Puente de Pre-SR al runtime de AMD: Entrada DLSS → AMD NR → FFX | Creó la **planificación multi-ranura (Multi-slot)**, eliminando **8.7 ms/fotograma** de bloqueos inactivos de la GPU; adaptó 0.3.1; restauró congelación/restauración de estados D3D12; mejoró compatibilidad con XBOX PC. **Sobrecarga del puente de solo 0.01–0.03 ms** |
| **[danielblnc / DLSS-NR-on-AMD](https://github.com/danielblnc/DLSS-NR-on-AMD)** | Runtime central de AMD Neural Rendering (0.3.0 / 0.3.1 / 0.3.2 / 0.3.3 / 0.4.0) | Invoca el runtime estándar sin modificaciones centrales; añade protección de estado D3D12 para la espera de dibujado de 1 píxel de 0.3.1+ |
| **[lmxxf / dlss5-on-amd-9070xt-porting](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting)** | Red de 71 bloques con ingeniería inversa portada a kernels abiertos AMD HIP | **Integrado en el framework proxy universal OptiScaler para admitir más juegos DLSS / XeSS**; implementó ejecución en la misma cola del fotograma; desarrolló el runtime independiente con C-ABI estandarizado (`LmxxfNrRuntime`); añadió controles deslizantes de ajuste de detalle/color en tiempo real |
| **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** | Addon de código abierto para HDR / Corrección de color | Origen de los algoritmos de composición de color en `dlssnr.hlsl` |

---

## 2. Guía de instalación

<details>
<summary><strong>📦 Haga clic para desplegar: Contenido del paquete</strong></summary>

| Archivo / Directorio | Propósito |
|---|---|
| `OptiScaler.dll` | Binario principal (se renombra durante la instalación al nombre de proxy elegido) |
| `OptiScaler.ini` | Archivo de configuración central (contiene opciones de doble backend en `[DlssNr]`) |
| `OptiScaler\` | Dependencias centrales (FFX, XeSS, Agility SDK, plugins) |
| `Setup.bat` / `Setup.ps1` | Instalador interactivo (**Haga doble clic en `Setup.bat`**) |
| `Uninstall_OptiScaler_NR.bat` / `.ps1` | Desinstalador seguro (se coloca automáticamente en el directorio del juego) |
| `tools\` | Utilidades internas de compilación y verificación |
| `Licenses\` | Licencias de código abierto de terceros |
| `README.md` / `README.en.md` / `README.es.md` | Documentación (Chino / Inglés / Español) |

> **Nota**: Para cumplir con las licencias y políticas de distribución upstream, este paquete **no incluye** binarios propietarios de NVIDIA, herramientas del instalador de danielblnc ni pesos de modelo no autorizados.

</details>

---

### Paso 1: Preparar los archivos del backend

Prepare cualquiera de los backends (o ambos para instalación conjunta):

#### Opción A: [Preparar archivos del backend `lmxxf`](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting) o [haga clic aquí](https://gofile.io/d/RyvcrDxz) para descargar los pesos
- `LmxxfNrRuntime.dll` (del lanzamiento del proyecto o del [repositorio upstream de lmxxf](https://github.com/lmxxf/dlss5-on-amd-9070xt-porting));
- Carpeta de módulos `lmxxf-modules\` (estructura de doble arquitectura que incluye subdirectorios `gfx1200` [serie 9060, experimental] y `gfx1201` [serie 9070, producción], con 24 módulos `.hsaco` cada uno y un total de 48 módulos; selección automática según GPU D3D12/HIP y migración automática desde versiones planas anteriores);
- Carpeta de shaders `shaders\` (con los archivos `.hlsl`);
- Carpeta de pesos `native-game-tiled-assets\` (se puede descargar [aquí](https://gofile.io/d/RyvcrDxz));
- Coloque estos elementos en la misma carpeta descomprimida junto a `Setup.bat`.

#### Opción B: [Preparar archivos del backend `danielblnc`](https://github.com/danielblnc/DLSS-NR-on-AMD/releases)
- `dlssnr_on_amd_setup.exe` y `nvngx_dlssnr.dll` (de los [Releases de danielblnc](https://github.com/danielblnc/DLSS-NR-on-AMD/releases); el instalador genera los pesos automáticamente);
- O los archivos pregenerados `version.dll` y `dlssnr_on_amd_weights.bin`;
- Colóquelos en la misma carpeta descomprimida junto a `Setup.bat`.

---

### Paso 2: Ejecutar el instalador (Recomendado)

1. Descomprima este lanzamiento en cualquier carpeta temporal;
2. Coloque los archivos de su backend junto a `Setup.bat`;
3. **Asegúrese de que el juego no se esté ejecutando**;
4. **Haga doble clic en `Setup.bat`**:
   - Seleccione el directorio del ejecutable de su juego (ej. `...\Binaries\Win64\`);
   - Si se detectan ambos backends, elija cuál instalar o instale ambos;
   - Seleccione el nombre del DLL proxy (predeterminado `dxgi.dll`, recomendado; también se admiten `winmm.dll`, `d3d12.dll`; **no use `dinput8.dll`**);
   - El instalador configura los proxies, elimina archivos duplicados en conflicto y configura `OptiScaler.ini`.

---

### Paso 3: Instalación manual

Si prefiere colocar los archivos manualmente:
1. Renombre `OptiScaler.dll` al nombre de proxy elegido (ej. `dxgi.dll`) y cópielo en el directorio del juego;
2. Copie `OptiScaler.ini` y la carpeta `OptiScaler\` en el directorio del juego;
3. **Desplegar archivos de backend**:
   - **Para `lmxxf`**: Copie `LmxxfNrRuntime.dll`, `lmxxf-modules\`, `shaders\` y `native-game-tiled-assets\` en el directorio del juego;
   - **Para `danielblnc`**: Duplique `version.dll` como `dlssnr_amd_pass1.dll`, `dlssnr_amd_pass2.dll`, `dlssnr_amd_pass3.dll`; copie `dlssnr_on_amd_weights.bin` en el directorio del juego (**no deje ningún archivo llamado `version.dll`** para evitar doble inyección);
4. En `OptiScaler.ini`, establezca `Enabled = true` en `[DlssNr]` y defina `NrBackend = lmxxf` o `NrBackend = daniel`.

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

## 3. Arquitectura de doble backend y pruebas de rendimiento

Este proyecto admite dos tecnologías distintas de backend de AMD Neural Rendering:

```
                            ┌──► [Backend lmxxf]  ──► HIP abierto / Misma cola del frame / Ajuste profundo
Entradas DLSS/XeSS del juego ──► OptiScaler ──┤
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
  - `NR slots`: Cantidad de búferes paralelos (2–5, predeterminado 3);
  - `Every-frame`: Fuerza la reducción de ruido en cada fotograma;
  - `New wait mode`: Alternador del modo de espera de congelación/restauración de estado de 0.3.1.

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

> **¿Dónde están los registros de lmxxf?**  
> A diferencia de `danielblnc` que escribe en un `dlssnr_on_amd.log` independiente, el backend `lmxxf` y su runtime C-ABI envían todos los mensajes de inicialización, telemetría y errores directamente a **`OptiScaler.log`** (y `amd_bridge.log`). No es necesario buscar archivos de registro separados.

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

#### Notas sobre Microsoft Store / XBOX PC
Debido a la virtualización del sistema de archivos de Windows, ciertos títulos de Store / Game Pass crean una carpeta **`_storage_`** junto al ejecutable. Revise esta carpeta si los registros o salidas no aparecen en el directorio principal del juego.

### 3. Formato para reportar problemas
Al reportar problemas, por favor incluya:
1. Nombre del DLL proxy utilizado (ej. `dxgi.dll`);
2. Backend seleccionado (`lmxxf` o `daniel`);
3. Modelo de GPU, versión del sistema operativo y versión del controlador AMD;
4. Título del juego, resolución de salida y modo FSR;
5. Archivos `.log` relevantes indicados anteriormente.

### 4. Problemas conocidos
- **UE5 (Palworld, Neverness to Everness y otros)**: Las versiones anteriores rechazaban todas las consultas y dejaban sin envolver las listas de comandos del juego creadas antes de la primera swapchain, haciendo que el backend `lmxxf` devolviera el color original. El código actual permite consultas completadas y envuelve antes las listas creadas por el ejecutable del juego. Las pruebas D3D12 pasan; el renderizado neuronal y la estabilidad de imagen aún requieren validación en los juegos.

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
- [**RenoDX / clshortfuse**](https://github.com/clshortfuse/renodx) — **Licencia MIT**: Algoritmos de composición de color en `dlssnr.hlsl`;
- [**Este proyecto (TheAutomatic / dlss-5-amd-project)**](https://github.com/TheAutomatic/dlss-5-amd-project) — **Licencia GPL-3.0**: Planificación multi-ranura, ejecución en la misma cola del fotograma, creación de runtime con C-ABI y PR upstream, congelación/restauración de estado 0.3.1, coexistencia de doble backend e instalador inteligente.

Esta distribución no contiene binarios propietarios de NVIDIA, herramientas del instalador de danielblnc ni pesos de modelo no autorizados. Por favor, respete todas las licencias upstream.
