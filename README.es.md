[中文](README.md) | [English](README.en.md) | **Español**

# OptScaler(NR)

> [!IMPORTANT]
> **🎉 Actualización de arquitectura v2.0**: ¡Este proyecto se ha actualizado por completo a la arquitectura **"OptiScaler oficial sin modificar + Doble plugin `.asi` independiente (`dlssnr-amd.asi` + `openrr-amd.asi`) + Lanzador gráfico en un clic"**!
> Si desea consultar la documentación anterior de la versión `1.x` (la rama integrada de 3 backends `lmxxf` / `danielblnc` / `mochizuki`), haga clic aquí: **[Documentación anterior en español (Legacy README)](docs/legacy-readme/README.es.md)**.

**Agradecimiento especial** a todos los usuarios por sus pruebas y comentarios.

Este repositorio incluye un lanzador gráfico (**`OptScalerNR.Launcher.exe`**) que escanea automáticamente las bibliotecas de Steam / Epic / GOG / Xbox, analiza las importaciones PE del `.exe` del juego para recomendar el DLL proxy óptimo, extrae y sintetiza los pesos de los modelos sin conexión en un clic, y permite elegir e instalar libremente **OptiScaler oficial sin modificar** junto con dos plugins `.asi` de renderizado neuronal para AMD: **DLSS Neural Rendering (`dlssnr-amd.asi`)** y **DLSS Ray Reconstruction (`openrr-amd.asi`)**.

**Página del proyecto: [github.com/TheAutomatic/dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)**

---

## Índice
- [1. Arquitectura principal y créditos](#1-arquitectura-principal-y-créditos)
- [2. ⚡ Guía de instalación rápida (Windows y Linux)](#2--guía-de-instalación-rápida-windows-y-linux)
- [3. Capacidades de los dos plugins `.asi`](#3-capacidades-de-los-dos-plugins-asi)
- [4. Atajos de teclado y menús en el juego](#4-atajos-de-teclado-y-menús-en-el-juego)
- [5. Reparación de compatibilidad, registro y desinstalación](#5-reparación-de-compatibilidad-registro-y-desinstalación)
- [6. Aviso de gratuidad, atribuciones y licencias](#6-aviso-de-gratuidad-atribuciones-y-licencias)

---

## 1. Arquitectura principal y créditos

En la arquitectura `2.0`, ya no modificamos ni recompilamos el host `OptiScaler.dll`; utilizamos el **host oficial sin modificar + plugins `.asi` independientes**:

| Componente | Origen y autor | Función y características |
|---|---|---|
| **[OptiScaler](https://github.com/optiscaler/OptiScaler)**<br>(Host oficial sin modificar) | Versión oficial upstream (empaquetada sin cambios y verificada por SHA-256) | Conversión universal de reescalado (DLSS/XeSS/FSR → FSR 3.1 / FSR 4 / XeSS) y generación de fotogramas (DLSSG → FSR3 FG / XeFG), cargando `OptiScaler\plugins\*.asi` |
| **`dlssnr-amd.asi`**<br>(Plugin DLSS Neural Rendering) | **[mochizuki0323 / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)**<br>**[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** | Ejecuta DLSS Neural Rendering en GPUs AMD **RX 9000 (RDNA4)** y **RX 7000 / 700M / 800M (RDNA3)**; soporta **D3D12 Direct-ELF** y **Vulkan ACO**, con estilos RenoDX y wilsjo2 |
| **`openrr-amd.asi`**<br>(Plugin DLSS Ray Reconstruction) | **[Storm / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)**<br>**[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** | Ejecuta la red **DLSS Ray Reconstruction (DLSS-RR)** de forma nativa en GPUs AMD **RX 9000 (RDNA4)**; soporta **D3D12 Direct-ELF** y **Vulkan**, coexistiendo con `dlssnr-amd.asi` |

---

## 2. ⚡ Guía de instalación rápida (Windows y Linux)

### Instalación con el lanzador gráfico en Windows (Recomendado)

1. **Descargar y descomprimir**: Descargue el archivo `.zip` desde [Releases](https://github.com/TheAutomatic/dlss-5-amd-project/releases) y descomprímalo por completo.
2. **Abrir el lanzador**: Haga doble clic en **`OptScalerNR.Launcher.exe`** para abrir **`OptScaler NR` Launcher**.
3. **Seleccionar juego y proxy de inyección**:
   - Elija el juego en la lista izquierda (o pulse **`+`** arriba a la derecha para elegir el `.exe` real del juego; en Unreal Engine suele ser `<Game>\Binaries\Win64\*-Shipping.exe`);
   - El lanzador analizará automáticamente la tabla PE y seleccionará el proxy recomendado (`dxgi.dll` o `winmm.dll`).
4. **Marcar los plugins `.asi` que desee instalar**:
   - **[☑] `dlssnr-amd.asi` (DLSS Neural Rendering · Mochizuki)**:
     - La primera vez, pulse **"Import model source file…"** y elija su `nvngx_dlssnr.dll` (`310.9.16` o `310.8.0`). El lanzador lo copiará en su carpeta y extraerá los modelos automáticamente para todos los juegos futuros.
   - **[☑] `openrr-amd.asi` (OpenRR Ray Reconstruction · RX 9000)**:
     - Al marcarlo, pulse **"Import nvngx_dlssd.dll…"** una sola vez y seleccione `nvngx_dlssd.dll` versión **`310.9.1.0`**.
   - **[☐] Instalar OptiPatcher (Opcional)**: Desbloquea opciones DLSS nativas en más de 270 juegos.
5. **Instalar en un clic**: Pulse el botón rojo **"Install / Update"** arriba a la derecha.

### Instalación en Linux / SteamOS (Proton)

```bash
# Instalación interactiva
bash install.sh "/ruta/a/la/carpeta/del/juego"

# Desinstalar / Empaquetar registros
bash install.sh "/ruta/a/la/carpeta/del/juego" remove
bash install.sh "/ruta/a/la/carpeta/del/juego" logs
```
En las **Opciones de lanzamiento** de Steam, añada: `WINEDLLOVERRIDES="dxgi=n,b" %command%`.

---

## 3. Capacidades de los dos plugins `.asi`

- **`dlssnr-amd.asi` (Mochizuki · DLSS Neural Rendering)**: Soporta **RX 9000 (RDNA4)** (las 4 redes: `CC_SMALL_FP8`, `CC_SMALL_FP8_EXPERIMENTAL`, `CC_SILVER_AARDWOLD`, `CC_SILVER_AARDWOLD_OPTIMIZED` con INT4) y **RX 7000 / 700M / 800M (RDNA3)** (`CC_SMALL_FP8` y `CC_SILVER_AARDWOLD`), con ejecución **D3D12 Direct-ELF** y **Vulkan**.
- **`openrr-amd.asi` (OpenRR · DLSS Ray Reconstruction)**: Soporta **RX 9000 (RDNA4)** (con `nvngx_dlssd.dll` `310.9.1.0`), ejecutando reconstrucción de rayos neuronal en **D3D12 Direct-ELF** y **Vulkan** sin conflictos con `dlssnr-amd.asi`.

---

## 4. Atajos de teclado y menús en el juego

| Tecla | Menú / Función | Descripción |
|---|---|---|
| **`Insert (Ins)`** | **Menú OptiScaler** + **Panel OpenRR** | Ajusta el reescalado y la generación de fotogramas; si `openrr-amd.asi` está instalado, el **panel de OpenRR se abre y cierra junto con `Insert`** |
| **`End`** | **NR Control (Panel Mochizuki NR)** | Ajusta todos los parámetros de `dlssnr-amd.asi`: cambio de red en vivo, estilo RenoDX / wilsjo2, Passes (1–3), HUD fix y diagnóstico |
| **`Ctrl + F11`** | Cambiar red NR | Alterna rápidamente entre las redes neuronales instaladas |
| **`F5`** | Captura comparativa NR | Guarda capturas con y sin NR en la carpeta `NR Screenshots\` del juego |

---

## 5. Reparación de compatibilidad, registro y desinstalación

- **Reparación de compatibilidad (XeSS / FidelityFX)**: Permite respaldar y actualizar `libxess.dll` / `libxell.dll` / `amd_fidelityfx_*.dll` del juego en un clic y restaurarlos cuando quiera.
- **Empaquetar registros (Pack Logs)**: Genera un archivo `.zip` en el Escritorio con `OptiScaler.log`, `dlssnr-amd.log`, `openrr-amd.log`, archivos `.ini` e información del sistema.
- **Desinstalación limpia**: Pulse **"Uninstall"** en el lanzador (o ejecute `Uninstall_OptiScaler_NR.bat` en la carpeta del juego) para eliminar todos los archivos instalados.

---

## 6. Aviso de gratuidad, atribuciones y licencias

Este software es completamente gratuito. Si pagó por él, solicite un reembolso de inmediato.

- **[OptiScaler](https://github.com/optiscaler/OptiScaler)** — **GPL-3.0 License**
- **[Mochizuki / DLSSNR-AMD](https://gitlab.com/mochizuki0323/DLSSNR-AMD)** & **[TheAutomatic / DLSSNR-AMD](https://github.com/TheAutomatic/DLSSNR-AMD)** (`dlssnr-amd.asi`) — **MIT License** (© mochizuki0323)
- **[Storm / OpenRR](https://github.com/OpenNeuralRendering/OpenRR)** & **[TheAutomatic / OpenRR-ASI](https://github.com/TheAutomatic/OpenRR-ASI)** (`openrr-amd.asi`) — **MIT License**
- **[RenoDX / clshortfuse](https://github.com/clshortfuse/renodx)** — **MIT License**
- **[OptiPatcher](https://github.com/optiscaler/OptiPatcher)** — **GPL-3.0 License**
- **[TheAutomatic / dlss-5-amd-project](https://github.com/TheAutomatic/dlss-5-amd-project)** — **GPL-3.0 License**
