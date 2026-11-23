/* OptiScaler NR - Linux launcher UI (no framework, no external resources).
 * Talks to the local launcher server: every /api/ call carries the per-session token. */
'use strict';

const QS = new URLSearchParams(location.search);
const TOKEN = QS.get('t') || '';
const OPT = {
  lang: QS.get('lang'),            // zh | en (overrides the saved setting, for screenshots/tests)
  theme: QS.get('theme'),          // light | dark | system
  select: QS.get('select'),        // game id to select at start
  noevents: QS.get('noevents') === '1',  // no event stream / heartbeat (headless screenshots)
  dialog: QS.get('dialog'),        // settings | confirm | browse  (open at start, for screenshots)
};

/* ------------------------------------------------------------------------------------------ i18n */
const STR = {
  en: {
    search: 'Search games', library: 'Library', addGame: 'Add a game folder', rescan: 'Rescan library',
    language: 'Language', theme: 'Theme', settings: 'Settings',
    src_steam: 'Steam', src_heroic: 'Heroic', src_lutris: 'Lutris', src_manual: 'Manual',
    st_installed: 'Installed', st_not: 'Not installed', st_foreign: 'Other mod found', st_noexe: 'No Windows exe',
    st_missing: 'Folder missing',
    r_optiscaler: 'OptiScaler', r_reshade: 'ReShade Feeder', r_vulkan: 'ReShade Feeder · Vulkan', r_dx9: 'ReShade Feeder · D3D9 depth',
    rs_optiscaler: 'OptiScaler', rs_reshade: 'ReShade D3D10–12', rs_vulkan: 'ReShade Vulkan/D3D9', rs_dx9: 'ReShade D3D9 depth',
    rd_optiscaler: 'Games with a DLSS, FSR or XeSS option. OptiScaler upscales, NR runs as its plugin. Best quality.',
    rd_reshade: 'D3D10 / 11 / 12 games without a usable upscaler. ReShade with VORT motion vectors.',
    rd_vulkan: 'Vulkan games, or D3D9 games (DXVK turns D3D9 into Vulkan under Proton).',
    rd_dx9: 'Same as Vulkan / D3D9, plus the depth direction that old D3D9 games need.',
    only64: 'x86_64 package only — OptiScaler is 64-bit. Use the 64-bit package for this route.',
    current: 'Installed',
    sec_route: 'Install route', sec_plugins: 'ASI plugins (OptiScaler)', sec_options: 'Options', sec_model: 'Model', sec_launch: 'Steam launch options',
    sec_output: 'Installer output',
    p_nr: 'Mochizuki NR (Neural Rendering)', p_nr_d: 'DLSS 5 Neural Rendering plugin (dlssnr-amd.asi). Requires nvngx_dlssnr.dll on first install.',
    p_openrr: 'OpenRR {ver} (Ray Reconstruction)',
    p_openrr_d: 'DLSS Ray Reconstruction on AMD RX 9000 (RDNA4) & experimental RX 7000 (RDNA3), Mesa RADV ≥ 26.3.',
    p_dlssd_yes: 'Native DLSSD detected', p_dlssd_no: 'No native DLSSD detected',
    p_spoof: 'Reduce global GPU spoofing (Dxgi=auto)',
    p_spoof_d: 'Keeps native AMD adapter identity when paired with OptiPatcher (experimental).',
    p_optipatcher: 'OptiPatcher', p_optipatcher_d: 'Optional DLSS spoofing enhancement plugin (OptiPatcher.asi).',
    dlssd_ok: 'nvngx_dlssd.dll cached in this package', dlssd_ok_d: 'Copied into openrr-amd/ automatically during install.',
    dlssd_game: 'Game folder already has nvngx_dlssd.dll', dlssd_game_d: 'Used automatically, or pick a 310.9.1.0 nvngx_dlssd.dll below to cache in this package.',
    dlssd_need: 'nvngx_dlssd.dll (310.9.1.0) not cached yet',
    dlssd_need_d: 'Optional now, required in game for OpenRR. Pick nvngx_dlssd.dll (310.9.1.0) or a zip containing it to cache in this package.',
    dlssd_ph: '/path/to/nvngx_dlssd.dll or .zip', other_dlssd: 'Use another DLL',
    o_model: 'Starting network',
    o_model_d: 'All four networks are installed and switchable in game (Ctrl+F11). This sets the one the game starts with.',
    m_CC_SMALL_FP8: 'CC_SMALL_FP8 — default (310.9.16)', m_CC_SMALL_FP8_EXPERIMENTAL: 'CC_SMALL_FP8_EXPERIMENTAL — faster, experimental',
    m_CC_SILVER_AARDWOLD: 'CC_SILVER_AARDWOLD — 310.8.0 network', m_CC_SILVER_AARDWOLD_OPTIMIZED: 'CC_SILVER_AARDWOLD_OPTIMIZED — faster (RX 9000)',
    o_style: 'Processing style', o_style_d: 'How the picture is prepared for NR and put back. The network itself is the same.',
    o_lum: 'LumeniteFX motion vectors', o_lum_d: 'ReShade routes. Downloaded once from its author’s GitHub (AGNYA licence); VORT otherwise.',
    o_lum_opti: 'Not used on the OptiScaler route — it uses the game’s own motion vectors.',
    on: 'On', off: 'Off',
    mdl_ok: 'Model cached in this package', mdl_ok_d: 'CC_SMALL_FP8 and CC_SILVER_AARDWOLD verified by SHA-256 — no DLL needed.',
    mdl_part: 'CC_SILVER_AARDWOLD cached, CC_SMALL_FP8 missing',
    mdl_part_d: 'Installs work; the CC_SMALL_FP8 networks fall back. Pick the 310.9.16 DLL once to add it (optional).',
    mdl_need: 'Model needed — first install only',
    mdl_need_d: 'Pick NVIDIA’s nvngx_dlssnr.dll (310.9.16 recommended, or 310.8.0) or a zip with it. The model is extracted and kept in this package.',
    mdl_bad: 'The cached model is damaged', mdl_bad_d: 'Pick nvngx_dlssnr.dll again to re-extract it.',
    dll_ph: '/path/to/nvngx_dlssnr.dll or .zip', browse: 'Browse…', other_dll: 'Use another DLL', hide: 'Hide',
    install: 'Install', reinstall: 'Reinstall', uninstall: 'Uninstall', packLogs: 'Pack logs', openFolder: 'Open folder',
    change: 'Change…', reset: 'Use detected', remove_entry: 'Remove from list', copy: 'Copy', copied: 'Copied to clipboard',
    launch_hint: 'Steam › game › Properties › General › Launch options — paste the whole line.',
    launch_main: 'Launch options', launch_alt: 'For CC_SILVER_AARDWOLD_OPTIMIZED with int4 layers', launch_guess: 'Inferred from the installed files',
    out_idle: 'Nothing yet. The installer’s output appears here.', clear: 'Clear',
    j_install: 'Installing', j_remove: 'Uninstalling', j_logs: 'Packing logs', j_done: 'Finished', j_failed: 'Failed',
    sum_install: 'Will install <b>{route}</b> into this folder', sum_re: 'Installed: <b>{route}</b> — reinstall replaces it',
    sum_need_model: 'Choose the model file first', sum_need_plugin: 'Enable Mochizuki NR or OpenRR first', sum_busy: 'Working…',
    w_bits: '{exe} is {b}-bit, but this is the {pb}-bit package. Its DLLs will not load — use the OptiScaler NR Linux {other} package.',
    w_bits_t: 'Bitness mismatch', w_foreign_t: 'Another mod', w_foreign: '{file} is in the game folder but was not installed by OptiScaler NR. Installing replaces it.',
    w_noexe_t: 'No game exe', w_noexe: 'No Windows .exe was found here. Use “Change…” to pick the folder that holds the game’s exe (for Unreal games: <project>/Binaries/Win64).',
    w_missing: 'This folder does not exist any more.', w_nopkg_t: 'Package not found', w_nopkg: 'install.sh was not found next to the launcher. Run OptiScalerNR-Launcher.sh from the extracted package.',
    ok_install: 'Installed {route}', ok_remove: 'Uninstalled', ok_logs: 'Logs packed', failed_t: 'Failed',
    ok_install_d: 'Add the Steam launch options below, then start the game.', ok_remove_d: 'Every file recorded in dlssnr-amd-install.txt was removed.',
    open_logs: 'Show file',
    cf_title: 'Uninstall from {name}?', cf_text: 'Every file listed in dlssnr-amd-install.txt is deleted, including INI files and logs. Game files that the installation overwrote are not restored.',
    cancel: 'Cancel', close: 'Close',
    set_appearance: 'Appearance', set_lang: 'Language', set_theme: 'Theme', light: 'Light', dark: 'Dark', system: 'System',
    set_pkg: 'Package', set_ver: 'Version', set_arch: 'Architecture', set_up: 'DLSSNR-AMD', set_openrr: 'OpenRR', set_path: 'Folder', set_model: 'Model cache',
    set_int: 'Integration', set_desktop: 'App menu entry', set_desktop_d: 'Adds “OptiScaler NR” to your application menu (~/.local/share/applications).',
    add: 'Add', remove: 'Remove', set_quit: 'Quit launcher', set_quit_d: 'Stops the local server. Closing this window does the same after a moment.',
    quit: 'Quit', open: 'Open',
    mc_ok: 'cached', mc_missing: 'missing', mc_bad: 'damaged',
    fb_dir: 'Choose the game folder', fb_model: 'Choose nvngx_dlssnr.dll or a zip', fb_dlssd: 'Choose nvngx_dlssd.dll (310.9.1.0) or a zip', fb_selDir: 'Use this folder', fb_select: 'Select',
    fb_up: 'Up', fb_go: 'Go', fb_hidden: 'Show hidden', fb_empty: 'This folder is empty.', fb_noitems: 'No .dll or .zip files here.',
    pl_home: 'Home', pl_downloads: 'Downloads', pl_desktop: 'Desktop', pl_documents: 'Documents', pl_steam: 'Steam library', pl_drive: 'Drive',
    lib_empty: 'No games found', lib_empty_d: 'Steam, Heroic and Lutris libraries are scanned automatically. You can add any game folder by hand.',
    no_match: 'No games match “{q}”.', select_game: 'Select a game', select_game_d: 'Pick a game on the left to install OptiScaler NR into it.',
    ready: 'Ready', games_n: '{n} games', inst_n: '{n} installed', scanning: 'Scanning libraries…',
    notice: 'This tool is free. If you paid for it, request a refund and report the seller.',
    stopped: 'The launcher has stopped', stopped_d: 'You can close this window.', lost: 'Connection to the launcher lost — retrying…',
    e_model_required: 'Choose nvngx_dlssnr.dll (or its zip) first — the package has no model yet.',
    e_plugin_required: 'Enable at least one plugin (Mochizuki NR or OpenRR).',
    e_openrr_unavailable: 'OpenRR is only available on the x86_64 OptiScaler route.',
    e_optipatcher_unavailable: 'OptiPatcher is only available on the x86_64 OptiScaler route.',
    e_dlssd_invalid: 'That is not an nvngx_dlssd.dll or .zip file.',
    e_route_unavailable: 'The OptiScaler route is only in the x86_64 package.', e_busy: 'Another operation is still running.',
    e_dll_invalid: 'That is not an nvngx_dlssnr.dll or .zip file.', e_folder_missing: 'The game folder does not exist.',
    bits: '{b}-bit', exe: 'exe', manualAdded: 'Game added', desktopAdded: 'Menu entry added', desktopRemoved: 'Menu entry removed',
    override_on: 'custom folder',
  },
  zh: {
    search: '搜索游戏', library: '游戏库', addGame: '添加游戏文件夹', rescan: '重新扫描游戏库',
    language: '语言', theme: '主题', settings: '设置',
    src_steam: 'Steam', src_heroic: 'Heroic', src_lutris: 'Lutris', src_manual: '手动',
    st_installed: '已安装', st_not: '未安装', st_foreign: '检测到其他模组', st_noexe: '未找到 Windows exe', st_missing: '文件夹不存在',
    r_optiscaler: 'OptiScaler', r_reshade: 'ReShade Feeder', r_vulkan: 'ReShade Feeder · Vulkan', r_dx9: 'ReShade Feeder · D3D9 深度',
    rs_optiscaler: 'OptiScaler', rs_reshade: 'ReShade D3D10–12', rs_vulkan: 'ReShade Vulkan/D3D9', rs_dx9: 'ReShade D3D9 深度',
    rd_optiscaler: '适用于有 DLSS、FSR 或 XeSS 选项的游戏。OptiScaler 负责超分，NR / OpenRR 作为其插件运行，画质最佳。',
    rd_reshade: '适用于没有可用超分选项的 D3D10 / 11 / 12 游戏。ReShade + VORT 运动矢量。',
    rd_vulkan: '适用于 Vulkan 游戏，或 D3D9 游戏（Proton 下 DXVK 会把 D3D9 转为 Vulkan）。',
    rd_dx9: '与 Vulkan / D3D9 相同，并加入老 D3D9 游戏所需的深度方向设置。',
    only64: '仅 x86_64 包提供 —— OptiScaler 只有 64 位。此路线请使用 64 位包。',
    current: '已安装',
    sec_route: '安装路线', sec_plugins: 'ASI 插件选择（神经渲染 & 光线重建）', sec_options: '选项', sec_model: '模型', sec_launch: 'Steam 启动选项', sec_output: '安装程序输出',
    p_nr: 'Mochizuki NR（神经渲染）', p_nr_d: 'DLSS 5 神经渲染插件（dlssnr-amd.asi），首次安装需要 nvngx_dlssnr.dll。',
    p_openrr: 'OpenRR {ver}（光线重建）',
    p_openrr_d: '适用于 RX 9000（RDNA4）及实验性 RX 7000（RDNA3），需 Mesa RADV ≥ 26.3（仅限光线重建游戏）。',
    p_dlssd_yes: '已检测到原生 DLSSD', p_dlssd_no: '未检测到原生 DLSSD',
    p_spoof: '减少全局显卡伪装（Dxgi=auto）',
    p_spoof_d: '实验性：配合 OptiPatcher 保持真实 A 卡身份。',
    p_optipatcher: 'OptiPatcher', p_optipatcher_d: '可选的 DLSS 伪装增强插件（OptiPatcher.asi）。',
    dlssd_ok: 'nvngx_dlssd.dll 已缓存在此包中', dlssd_ok_d: '安装 OpenRR 时会自动复制到游戏 openrr-amd/ 目录。',
    dlssd_game: '游戏目录中已有 nvngx_dlssd.dll', dlssd_game_d: '安装时会自动复制，也可在下方选择 310.9.1.0 版缓存到此包中。',
    dlssd_need: '尚未缓存 nvngx_dlssd.dll（310.9.1.0）',
    dlssd_need_d: '现在可选，但游戏内启用 OpenRR 需要它。选择 nvngx_dlssd.dll（310.9.1.0）或包含它的 zip 即可缓存到此包。',
    dlssd_ph: '/路径/nvngx_dlssd.dll 或 .zip', other_dlssd: '使用其他 DLL',
    o_model: '启动网络', o_model_d: '四个网络都会安装，游戏中可随时切换（Ctrl+F11）。此处仅设置游戏启动时使用的网络。',
    m_CC_SMALL_FP8: 'CC_SMALL_FP8 —— 默认（310.9.16）', m_CC_SMALL_FP8_EXPERIMENTAL: 'CC_SMALL_FP8_EXPERIMENTAL —— 更快，实验性',
    m_CC_SILVER_AARDWOLD: 'CC_SILVER_AARDWOLD —— 310.8.0 网络', m_CC_SILVER_AARDWOLD_OPTIMIZED: 'CC_SILVER_AARDWOLD_OPTIMIZED —— 更快（RX 9000）',
    o_style: '处理风格', o_style_d: 'NR 前后的画面处理方式，NR 网络本身相同。',
    o_lum: 'LumeniteFX 运动矢量', o_lum_d: '仅 ReShade 路线。首次会从作者 GitHub 下载（AGNYA 许可）；关闭时使用 VORT。',
    o_lum_opti: 'OptiScaler 路线不使用 —— 它直接使用游戏自身的运动矢量。',
    on: '开', off: '关',
    mdl_ok: '模型已缓存在此包中', mdl_ok_d: 'CC_SMALL_FP8 与 CC_SILVER_AARDWOLD 已通过 SHA-256 校验，无需 DLL。',
    mdl_part: '已缓存 CC_SILVER_AARDWOLD，缺少 CC_SMALL_FP8', mdl_part_d: '可以正常安装，CC_SMALL_FP8 系列会回退。选择一次 310.9.16 的 DLL 即可补齐（可选）。',
    mdl_need: '需要模型 —— 仅首次安装', mdl_need_d: '请选择 NVIDIA 的 nvngx_dlssnr.dll（推荐 310.9.16，或 310.8.0）或包含它的 zip。模型会被提取并保存在此包中。',
    mdl_bad: '缓存的模型已损坏', mdl_bad_d: '请重新选择 nvngx_dlssnr.dll 以重新提取。',
    dll_ph: '/路径/nvngx_dlssnr.dll 或 .zip', browse: '浏览…', other_dll: '使用其他 DLL', hide: '收起',
    install: '安装', reinstall: '重新安装', uninstall: '卸载', packLogs: '打包日志', openFolder: '打开文件夹',
    change: '更改…', reset: '使用自动检测', remove_entry: '从列表移除', copy: '复制', copied: '已复制到剪贴板',
    launch_hint: 'Steam › 游戏 › 属性 › 通用 › 启动选项 —— 粘贴整行。',
    launch_main: '启动选项', launch_alt: '使用 CC_SILVER_AARDWOLD_OPTIMIZED 的 int4 层时', launch_guess: '根据已安装文件推断',
    out_idle: '暂无输出。安装程序的输出会显示在这里。', clear: '清空',
    j_install: '正在安装', j_remove: '正在卸载', j_logs: '正在打包日志', j_done: '已完成', j_failed: '失败',
    sum_install: '将把 <b>{route}</b> 安装到此文件夹', sum_re: '已安装：<b>{route}</b> —— 重新安装将替换它',
    sum_need_model: '请先选择模型文件', sum_need_plugin: '请至少启用 Mochizuki NR 或 OpenRR', sum_busy: '处理中…',
    w_bits: '{exe} 是 {b} 位程序，而这是 {pb} 位包，其 DLL 无法加载 —— 请使用 OptiScaler NR Linux {other} 包。',
    w_bits_t: '位数不匹配', w_foreign_t: '其他模组', w_foreign: '游戏文件夹中有 {file}，但并非 OptiScaler NR 安装。安装会替换它。',
    w_noexe_t: '未找到游戏 exe', w_noexe: '此处没有找到 Windows .exe。请用“更改…”选择游戏 exe 所在的文件夹（虚幻引擎游戏：<项目>/Binaries/Win64）。',
    w_missing: '此文件夹已不存在。', w_nopkg_t: '未找到安装包', w_nopkg: '启动器旁没有找到 install.sh。请从解压后的包中运行 OptiScalerNR-Launcher.sh。',
    ok_install: '已安装 {route}', ok_remove: '已卸载', ok_logs: '日志已打包', failed_t: '失败',
    ok_install_d: '将下方的 Steam 启动选项填入后即可启动游戏。', ok_remove_d: 'dlssnr-amd-install.txt 中记录的所有文件均已删除。',
    open_logs: '显示文件',
    cf_title: '从 {name} 卸载？', cf_text: '将删除 dlssnr-amd-install.txt 中列出的所有文件（包括配置文件和日志）。安装时被覆盖的游戏文件不会恢复。',
    cancel: '取消', close: '关闭',
    set_appearance: '外观', set_lang: '语言', set_theme: '主题', light: '浅色', dark: '深色', system: '跟随系统',
    set_pkg: '安装包', set_ver: '版本', set_arch: '架构', set_up: 'DLSSNR-AMD', set_openrr: 'OpenRR', set_path: '位置', set_model: '模型缓存',
    set_int: '系统集成', set_desktop: '应用菜单项', set_desktop_d: '将“OptiScaler NR”添加到应用菜单（~/.local/share/applications）。',
    add: '添加', remove: '移除', set_quit: '退出启动器', set_quit_d: '停止本地服务。关闭此窗口片刻后也会自动退出。',
    quit: '退出', open: '打开',
    mc_ok: '已缓存', mc_missing: '缺失', mc_bad: '已损坏',
    fb_dir: '选择游戏文件夹', fb_model: '选择 nvngx_dlssnr.dll 或 zip', fb_dlssd: '选择 nvngx_dlssd.dll（310.9.1.0）或 zip', fb_selDir: '使用此文件夹', fb_select: '选择',
    fb_up: '上一级', fb_go: '前往', fb_hidden: '显示隐藏文件', fb_empty: '此文件夹为空。', fb_noitems: '这里没有 .dll 或 .zip 文件。',
    pl_home: '主目录', pl_downloads: '下载', pl_desktop: '桌面', pl_documents: '文档', pl_steam: 'Steam 库', pl_drive: '磁盘',
    lib_empty: '没有找到游戏', lib_empty_d: '会自动扫描 Steam、Heroic 和 Lutris 游戏库，也可以手动添加任意游戏文件夹。',
    no_match: '没有与“{q}”匹配的游戏。', select_game: '选择一个游戏', select_game_d: '在左侧选择游戏，即可为其安装 OptiScaler NR。',
    ready: '就绪', games_n: '{n} 个游戏', inst_n: '{n} 个已安装', scanning: '正在扫描游戏库…',
    notice: '本工具完全免费，如果你是付费购买的，请立即退款并举报',
    stopped: '启动器已停止', stopped_d: '现在可以关闭此窗口。', lost: '与启动器的连接已断开，正在重试…',
    e_model_required: '请先选择 nvngx_dlssnr.dll（或其 zip）—— 安装包中还没有模型。',
    e_plugin_required: '请至少启用 Mochizuki NR 或 OpenRR 其中一个插件。',
    e_openrr_unavailable: 'OpenRR 仅在 x86_64 包的 OptiScaler 路线中提供。',
    e_optipatcher_unavailable: 'OptiPatcher 仅在 x86_64 包的 OptiScaler 路线中提供。',
    e_dlssd_invalid: '这不是 nvngx_dlssd.dll 或 .zip 文件。',
    e_route_unavailable: 'OptiScaler 路线仅在 x86_64 包中提供。', e_busy: '另一个操作仍在进行中。',
    e_dll_invalid: '这不是 nvngx_dlssnr.dll 或 .zip 文件。', e_folder_missing: '游戏文件夹不存在。',
    bits: '{b} 位', exe: 'exe', manualAdded: '已添加游戏', desktopAdded: '已添加菜单项', desktopRemoved: '已移除菜单项',
    override_on: '自定义文件夹',
  },
};
const ROUTE_KEY = { 'optiscaler': 'optiscaler', 'reshade-feeder': 'reshade', 'reshade-feeder-vulkan': 'vulkan', 'reshade-feeder-dx9': 'dx9' };
const ROUTES = Object.keys(ROUTE_KEY);
const MODELS = ['CC_SMALL_FP8', 'CC_SMALL_FP8_EXPERIMENTAL', 'CC_SILVER_AARDWOLD', 'CC_SILVER_AARDWOLD_OPTIMIZED'];

/* ----------------------------------------------------------------------------------------- icons */
const P = {
  search: '<circle cx="10.5" cy="10.5" r="6.25"/><path d="m15.2 15.2 4.8 4.8"/>',
  refresh: '<path d="M19.25 12a7.25 7.25 0 1 1-2.12-5.13"/><path d="M19.25 4.75v4.5h-4.5"/>',
  globe: '<circle cx="12" cy="12" r="8.25"/><path d="M3.75 12h16.5"/><path d="M12 3.75c2.3 2.25 3.45 5 3.45 8.25S14.3 18 12 20.25C9.7 18 8.55 15.25 8.55 12S9.7 6 12 3.75z"/>',
  theme: '<circle cx="12" cy="12" r="8.25"/><path d="M12 3.75a8.25 8.25 0 0 1 0 16.5z" fill="currentColor" stroke="none"/>',
  gear: '<path d="M18.75 10.13 21.08 10.52 21.08 13.48 18.75 13.87 18.09 15.45 19.47 17.37 17.37 19.47 15.45 18.09 13.87 18.75 13.48 21.08 10.52 21.08 10.13 18.75 8.55 18.09 6.63 19.47 4.53 17.37 5.91 15.45 5.25 13.87 2.92 13.48 2.92 10.52 5.25 10.13 5.91 8.55 4.53 6.63 6.63 4.53 8.55 5.91 10.13 5.25 10.52 2.92 13.48 2.92 13.87 5.25 15.45 5.91 17.37 4.53 19.47 6.63 18.09 8.55Z"/><circle cx="12" cy="12" r="3"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  folder: '<path d="M3.75 7.25a2 2 0 0 1 2-2h3.6l2 2h6.9a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2H5.75a2 2 0 0 1-2-2z"/>',
  file: '<path d="M6.75 3.75h6.5l4 4v12.5H6.75z"/><path d="M13.25 3.75v4h4"/>',
  copy: '<rect x="8.75" y="8.75" width="11.5" height="11.5" rx="2"/><path d="M15.25 8.75V5.75a2 2 0 0 0-2-2h-7.5a2 2 0 0 0-2 2v7.5a2 2 0 0 0 2 2h3"/>',
  check: '<path d="m5 12.5 4.5 4.5L19 7.5"/>',
  checkCircle: '<circle cx="12" cy="12" r="8.25"/><path d="m8.25 12.25 2.5 2.5 5-5"/>',
  info: '<circle cx="12" cy="12" r="8.25"/><path d="M12 11v5.25"/><path d="M12 7.75v.01" stroke-width="2"/>',
  warn: '<path d="M10.3 4.6 3.4 17a2 2 0 0 0 1.7 3h13.8a2 2 0 0 0 1.7-3L13.7 4.6a2 2 0 0 0-3.4 0z"/><path d="M12 9.5v4"/><path d="M12 16.75v.01" stroke-width="2"/>',
  error: '<circle cx="12" cy="12" r="8.25"/><path d="m9.25 9.25 5.5 5.5M14.75 9.25l-5.5 5.5"/>',
  x: '<path d="m6.5 6.5 11 11M17.5 6.5l-11 11"/>',
  up: '<path d="M12 19V5"/><path d="m6.5 10.5 5.5-5.5 5.5 5.5"/>',
  layers: '<path d="m12 4 8.25 4.25L12 12.5 3.75 8.25z"/><path d="m3.75 12 8.25 4.25L20.25 12"/><path d="m3.75 15.75 8.25 4.25 8.25-4.25"/>',
  monitor: '<rect x="3.25" y="4.75" width="17.5" height="11.5" rx="2"/><path d="M8.5 19.75h7"/><path d="m12 8 .9 1.85 1.85.9-1.85.9L12 13.5l-.9-1.85-1.85-.9 1.85-.9z"/>',
  vulkan: '<path d="M4.25 5.5 12 19.25 19.75 5.5"/><path d="M8.6 5.5 12 11.6l3.4-6.1"/>',
  depth: '<rect x="3.75" y="8.75" width="11.5" height="11.5" rx="1.75"/><path d="M8 5.25h9.25a2 2 0 0 1 2 2v9.25"/><path d="M11 2.25h8.75a2 2 0 0 1 2 2v.01" opacity=".6"/>',
  chip: '<rect x="6.25" y="6.25" width="11.5" height="11.5" rx="2"/><rect x="9.5" y="9.5" width="5" height="5" rx="1"/><path d="M9.5 2.75v3.5M14.5 2.75v3.5M9.5 17.75v3.5M14.5 17.75v3.5M2.75 9.5h3.5M2.75 14.5h3.5M17.75 9.5h3.5M17.75 14.5h3.5"/>',
  wand: '<path d="m4.5 19.5 10-10"/><path d="m13 8 3 3"/><path d="M17.5 3.5v3M16 5h3M20 9.5v2M19 10.5h2M9.5 3.75v2M8.5 4.75h2"/>',
  motion: '<path d="M3.75 12h10.5"/><path d="M3.75 7.25h6.5"/><path d="M3.75 16.75h6.5"/><path d="m15.25 7.75 4.5 4.25-4.5 4.25"/>',
  terminal: '<rect x="3.25" y="4.75" width="17.5" height="14.5" rx="2"/><path d="m7.5 9.75 2.75 2.25-2.75 2.25"/><path d="M12.75 14.5h3.75"/>',
  box: '<path d="M3.75 7.5 12 3.75l8.25 3.75v9L12 20.25 3.75 16.5z"/><path d="M3.75 7.5 12 11.25l8.25-3.75M12 11.25v9"/>',
  trash: '<path d="M4.75 6.75h14.5"/><path d="M9.25 6.75V4.75h5.5v2"/><path d="M6.5 6.75l.85 12.1a1.5 1.5 0 0 0 1.5 1.4h6.3a1.5 1.5 0 0 0 1.5-1.4l.85-12.1"/>',
  download: '<path d="M12 4v11"/><path d="m7.25 10.5 4.75 4.75 4.75-4.75"/><path d="M4.75 19.25h14.5"/>',
  shield: '<path d="M12 3.5 5.25 6v5.5c0 4.2 2.9 7.6 6.75 9 3.85-1.4 6.75-4.8 6.75-9V6z"/><path d="m9.25 12 2 2 3.5-3.75"/>',
  gamepad: '<path d="M7.5 7.75h9a4.25 4.25 0 0 1 4.1 5.35l-.9 3.4a2.3 2.3 0 0 1-3.9 1.05l-1.85-2.05h-4.9l-1.85 2.05a2.3 2.3 0 0 1-3.9-1.05l-.9-3.4A4.25 4.25 0 0 1 7.5 7.75z"/><path d="M8.25 10.75v3M6.75 12.25h3"/><path d="M15.5 11.25v.01M17 13v.01" stroke-width="2"/>',
  drive: '<rect x="3.25" y="6.25" width="17.5" height="11.5" rx="2"/><path d="M7 14h.01M10 14h.01" stroke-width="2"/>',
  home: '<path d="m4 10.5 8-6.25 8 6.25"/><path d="M6.25 9v10.25h11.5V9"/>',
  power: '<path d="M12 3.75v7.5"/><path d="M7.4 6.6a7.25 7.25 0 1 0 9.2 0"/>',
  menu: '<rect x="3.75" y="3.75" width="7" height="7" rx="1.5"/><rect x="13.25" y="3.75" width="7" height="7" rx="1.5"/><rect x="3.75" y="13.25" width="7" height="7" rx="1.5"/><rect x="13.25" y="13.25" width="7" height="7" rx="1.5"/>',
  exe: '<rect x="4.75" y="4.75" width="14.5" height="14.5" rx="2.5"/><path d="M4.75 9.25h14.5"/>',
};
const icon = (n, cls = '') => `<svg class="i ${cls}" viewBox="0 0 24 24" aria-hidden="true">${P[n] || ''}</svg>`;
const ROUTE_ICON = { 'optiscaler': 'layers', 'reshade-feeder': 'monitor', 'reshade-feeder-vulkan': 'vulkan', 'reshade-feeder-dx9': 'depth' };
const LOGO = '<svg class="logo" viewBox="0 0 64 64" aria-hidden="true"><defs><linearGradient id="lg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#F23A4F"/><stop offset="1" stop-color="#C20E24"/></linearGradient></defs><rect x="2" y="2" width="60" height="60" rx="14" fill="url(#lg)"/><circle cx="32" cy="32" r="15" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="3"/><path d="M32 13c1.6 9.2 4.3 12.9 13.9 14.6 1.2.2 1.2 1.9 0 2.1C36.3 31.4 33.6 35.1 32 44.3c-.2 1.2-1.9 1.2-2.1 0-1.6-9.2-4.3-12.9-13.9-14.6-1.2-.2-1.2-1.9 0-2.1 9.6-1.7 12.3-5.4 13.9-14.6.2-1.2 1.9-1.2 2.1 0z" fill="#fff" transform="translate(1 3.3) scale(.97)"/></svg>';

/* ----------------------------------------------------------------------------------------- state */
const S = {
  info: null, games: [], loaded: false, scanning: false, selId: null, search: '',
  lang: 'en', themePref: 'system', forms: {}, job: null, jobLines: [], handled: new Set(), firstJobEvent: true,
  flash: {}, dialog: null, stopped: false, connected: true, status: null,
};

const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
function t(key, vars) {
  let s = (STR[S.lang] && STR[S.lang][key]) || STR.en[key] || key;
  if (vars) s = s.replace(/\{(\w+)\}/g, (_, k) => (vars[k] == null ? '' : vars[k]));
  return s;
}
const tv = (key, vars) => { // like t(), with variables HTML-escaped but the template's own markup kept
  const safe = {};
  for (const k in vars || {}) safe[k] = esc(vars[k]);
  return t(key, safe);
};
const $ = (sel, root = document) => root.querySelector(sel);
const routeName = (r, short) => t((short ? 'rs_' : 'r_') + ROUTE_KEY[r]);

/* ------------------------------------------------------------------------------------------- api */
class ApiError extends Error { constructor(code, message, status) { super(message); this.code = code; this.status = status; } }
async function api(path, body) {
  const opts = { method: body === undefined ? 'GET' : 'POST', headers: { 'X-NR-Token': TOKEN }, cache: 'no-store' };
  if (body !== undefined) { opts.headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(body); }
  let res;
  try { res = await fetch(path, opts); } catch (e) { throw new ApiError('network', String(e), 0); }
  let data = null;
  try { data = await res.json(); } catch (e) { /* empty */ }
  if (!res.ok) throw new ApiError((data && data.error) || 'http', (data && data.message) || res.statusText, res.status);
  return data;
}
const artUrl = (id, kind) => `/api/art?id=${encodeURIComponent(id)}&kind=${kind}&t=${encodeURIComponent(TOKEN)}`;
function errText(e) { const k = 'e_' + (e && e.code); return STR.en[k] ? t(k) : (e && e.message) || String(e); }

/* ----------------------------------------------------------------------------------------- theme */
const mql = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;
function applyTheme() {
  const pref = S.themePref;
  const dark = pref === 'dark' || (pref === 'system' && mql && mql.matches);
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
}
if (mql) (mql.addEventListener ? mql.addEventListener('change', applyTheme) : mql.addListener(applyTheme));

/* ------------------------------------------------------------------------------------- helpers */
function hashHue(s) { let h = 0; for (const c of String(s)) h = (h * 31 + c.codePointAt(0)) >>> 0; return h % 360; }
function initial(name) { const m = String(name || '?').match(/[\p{L}\p{N}]/u); return (m ? m[0] : '?').toUpperCase(); }
function tileStyle(name, ink = true) {
  const h = hashHue(name);
  return `background:linear-gradient(135deg,hsl(${h} var(--tile-sat) var(--tile-l1)),hsl(${(h + 36) % 360} var(--tile-sat) var(--tile-l2)));` +
    (ink ? `color:hsl(${h} 28% var(--tile-ink));` : '');
}
function fmtSize(n) { if (n == null) return ''; const u = ['B', 'KB', 'MB', 'GB']; let i = 0; while (n >= 1024 && i < 3) { n /= 1024; i++; } return (i ? n.toFixed(1) : n) + ' ' + u[i]; }
const selected = () => S.games.find((g) => g.id === S.selId) || null;
const pkg = () => (S.info && S.info.package) || { ok: false, routes: {}, bits: 64, arch: 'x86_64', version: '' };
const busy = () => S.job && S.job.state === 'running';

function gameState(g) {
  if (g.missing) return { cls: 'bad', key: 'st_missing' };
  if (g.status.installed) return { cls: 'ok', key: 'st_installed', route: g.status.route };
  if (!g.exe) return { cls: 'warn', key: 'st_noexe' };
  if (g.status.foreign) return { cls: 'warn', key: 'st_foreign' };
  return { cls: '', key: 'st_not' };
}
function shortStatus(g) {
  const st = gameState(g);
  return st.route ? `${t(st.key)} · ${routeName(st.route, true)}` : t(st.key);
}

function formFor(g) {
  if (!S.forms[g.id]) {
    const p = pkg();
    const routes = p.routes || {};
    const c = g.choices || {};
    const st = g.status || {};
    const hasOpenRr = !!(p.openrr && p.openrr.available);
    let route = c.route || st.route || (routes.optiscaler && routes.optiscaler.available ? 'optiscaler' : 'reshade-feeder');
    if (routes[route] && !routes[route].available) route = 'reshade-feeder';
    const defOpenRr = hasOpenRr && (
      c.openrr !== undefined ? !!c.openrr :
      st.installed ? !!st.openrr : !!g.hasNativeDlssd
    );
    const defNr = c.nr !== undefined ? !!c.nr : (st.installed && st.route === 'optiscaler' ? !!st.nr : true);
    S.forms[g.id] = {
      route,
      model: c.model || st.model || 'CC_SMALL_FP8',
      style: c.style || st.style || 'renodx',
      lumenite: !!c.lumenite,
      nr: defNr,
      openrr: defOpenRr,
      openrrSpoof: c.openrrSpoof || st.openrrSpoof || 'full',
      optipatcher: !!(p.optipatcher && (c.optipatcher !== undefined ? c.optipatcher : st.optipatcher)),
      dll: '', showDll: false,
      dlssd: '', showDlssd: false,
    };
    if (!MODELS.includes(S.forms[g.id].model)) S.forms[g.id].model = 'CC_SMALL_FP8';
  }
  return S.forms[g.id];
}

function inferredLaunch(route) {
  if (!route) return null;
  return route === 'reshade-feeder-vulkan' || route === 'reshade-feeder-dx9'
    ? 'WINEDLLOVERRIDES="winevulkan=n,b;vulkan-1=n,b" %command%'
    : 'WINEDLLOVERRIDES="dxgi=n,b" %command%';
}

/* ------------------------------------------------------------------------------------- rendering */
function renderShell() {
  const p = pkg();
  $('#app').innerHTML = `
    <header class="topbar">
      <div class="brand">${LOGO}<span class="name">OptiScaler NR</span>
        <span class="ver" id="ver">${esc(p.version ? 'v' + p.version : '')}</span>
        <span class="arch" id="arch" title="Linux ${esc(p.arch)}">Linux ${esc(p.arch)}</span></div>
      <label class="search">${icon('search')}<input id="search" type="search" autocomplete="off" spellcheck="false" placeholder="${esc(t('search'))}" value="${esc(S.search)}"><span class="kbd">Ctrl F</span></label>
      <div class="actions">
        <button class="icon-btn ${S.scanning ? 'spin' : ''}" id="btn-rescan" data-act="rescan" title="${esc(t('rescan'))}" aria-label="${esc(t('rescan'))}">${icon('refresh')}</button>
        <button class="icon-btn" data-act="lang" title="${esc(t('language'))}" aria-label="${esc(t('language'))}"><span class="lang-badge">${S.lang === 'zh' ? '中' : 'EN'}</span></button>
        <button class="icon-btn" data-act="theme" title="${esc(t('theme'))} · ${esc(t(S.themePref))}" aria-label="${esc(t('theme'))}">${icon('theme')}</button>
        <button class="icon-btn" data-act="settings" title="${esc(t('settings'))}" aria-label="${esc(t('settings'))}">${icon('gear')}</button>
      </div>
    </header>
    <div class="main">
      <aside class="library" aria-label="${esc(t('library'))}">
        <div class="lib-head"><h2>${esc(t('library'))}</h2><span class="count" id="lib-count"></span><span class="grow"></span>
          <button class="icon-btn sm" data-act="add-game" title="${esc(t('addGame'))}" aria-label="${esc(t('addGame'))}">${icon('plus')}</button></div>
        <div class="lib-list" id="lib-list" role="listbox"></div>
      </aside>
      <main class="detail" id="detail"></main>
    </div>
    <footer class="statusbar"><div class="left" id="status-left"></div>
      <div class="notice" title="${esc(t('notice'))}">${icon('shield')}<span>${esc(t('notice'))}</span></div></footer>`;
  document.documentElement.lang = S.lang === 'zh' ? 'zh-CN' : 'en';
  renderLibrary(); renderDetail(); renderStatus();
}

function filteredGames() {
  const q = S.search.trim().toLowerCase();
  if (!q) return S.games;
  return S.games.filter((g) => g.name.toLowerCase().includes(q) || (g.folder || '').toLowerCase().includes(q) || t('src_' + g.source).toLowerCase().includes(q));
}

function renderLibrary() {
  const list = $('#lib-list');
  if (!list) return;
  $('#lib-count').textContent = S.loaded ? String(S.games.length) : '';
  if (!S.loaded) {
    list.innerHTML = `<div class="skeleton">${'<div class="row"><div class="b" style="height:40px"></div><div><div class="b" style="height:12px;width:80%;margin:6px 0 8px"></div><div class="b" style="height:10px;width:50%"></div></div></div>'.repeat(6)}</div>`;
    return;
  }
  const games = filteredGames();
  if (!S.games.length) {
    list.innerHTML = `<div class="lib-empty">${esc(t('lib_empty'))}<br><br><button class="btn sm" data-act="add-game">${icon('plus', 'sm')}${esc(t('addGame'))}</button></div>`;
    return;
  }
  if (!games.length) { list.innerHTML = `<div class="lib-empty">${esc(t('no_match', { q: S.search }))}</div>`; return; }
  list.innerHTML = games.map((g) => {
    const st = gameState(g);
    const thumb = g.art.thumb || g.art.hero
      ? `<img loading="lazy" alt="" src="${artUrl(g.id, 'thumb')}" data-fallback="${esc(initial(g.name))}">`
      : `<div class="tile" style="${tileStyle(g.name)}">${esc(initial(g.name))}</div>`;
    return `<button class="item ${g.id === S.selId ? 'sel' : ''}" role="option" aria-selected="${g.id === S.selId}" data-act="select" data-id="${esc(g.id)}" title="${esc(g.name)}">
      <div class="thumb">${thumb}</div>
      <div style="min-width:0"><div class="l1">${esc(g.name)}</div>
        <div class="l2"><span class="tag">${esc(t('src_' + g.source))}</span><span class="dot ${st.cls}"></span><span class="txt">${esc(shortStatus(g))}</span></div></div>
    </button>`;
  }).join('');
}

function infobar(kind, title, text, extra = '') {
  const ic = { ok: 'checkCircle', warn: 'warn', bad: 'error', info: 'info' }[kind];
  return `<div class="infobar ${kind}">${icon(ic)}<div><span class="ib-t">${esc(title)}</span><span class="ib-d">${text}</span></div>${extra || '<span></span>'}</div>`;
}

function renderDetail() {
  const host = $('#detail');
  if (!host) return;
  const scrollTop = host.scrollTop;
  const g = selected();
  const p = pkg();
  if (!g) {
    host.innerHTML = S.loaded && !S.games.length
      ? `<div class="placeholder"><div><div class="big">${icon('gamepad')}</div><h2>${esc(t('lib_empty'))}</h2><p>${esc(t('lib_empty_d'))}</p><button class="btn" data-act="add-game">${icon('plus', 'sm')}${esc(t('addGame'))}</button></div></div>`
      : `<div class="placeholder"><div><div class="big">${icon('gamepad')}</div><h2>${esc(t(S.loaded ? 'select_game' : 'scanning'))}</h2><p>${esc(S.loaded ? t('select_game_d') : '')}</p></div></div>`;
    return;
  }
  const f = formFor(g);
  const st = gameState(g);
  const routes = p.routes || {};
  const model = (S.info && S.info.model) || { ready: false };
  const dlssdInfo = (S.info && S.info.dlssd) || { cached: false };
  const hasOpenRr = !!(p.openrr && p.openrr.available);
  const hasOptiPatcher = !!p.optipatcher;
  const installed = g.status.installed;
  const jobHere = S.job && S.job.meta && S.job.meta.gameId === g.id;
  const running = busy();

  // hero
  const hasArt = g.art.hero || g.art.thumb;
  const heroBg = hasArt
    ? `<div class="bg" style="background-image:url('${artUrl(g.id, 'hero')}')"></div>`
    : `<div class="bg" style="${tileStyle(g.name, false)}"></div><div class="wm" style="color:hsl(${hashHue(g.name)} 30% var(--tile-ink));opacity:.16">${esc(initial(g.name))}</div>`;
  const bitsTag = g.exeBits ? `<span class="tag" title="${esc(g.exe || '')}">${esc(t('bits', { b: g.exeBits }))} ${esc(t('exe'))}</span>` : '';
  const installedPlugins = [];
  if (installed && g.status.route === 'optiscaler') {
    if (g.status.nr) installedPlugins.push('NR');
    if (g.status.openrr) installedPlugins.push('OpenRR');
    if (g.status.optipatcher) installedPlugins.push('OptiPatcher');
  }
  const stateLine = st.route
    ? `<span class="dot ok"></span><b>${esc(t('st_installed'))}</b><span>·</span><span>${esc(routeName(st.route))}</span>${installedPlugins.length ? `<span>·</span><span>${esc(installedPlugins.join(' + '))}</span>` : ''}${g.status.model && g.status.nr ? `<span>·</span><span>${esc(g.status.model)}</span>` : ''}`
    : `<span class="dot ${st.cls}"></span><span>${esc(t(st.key))}</span>`;

  // alerts
  const alerts = [];
  if (!p.ok) alerts.push(infobar('bad', t('w_nopkg_t'), esc(t('w_nopkg'))));
  const flash = S.flash[g.id];
  if (flash) {
    const close = `<button class="icon-btn sm ib-x" data-act="dismiss" title="${esc(t('close'))}">${icon('x', 'sm')}</button>`;
    if (flash.ok) {
      const extra = flash.logs ? `<div style="display:flex;gap:4px"><button class="btn sm" data-act="open-logs" data-path="${esc(flash.logs)}">${esc(t('open_logs'))}</button>${close}</div>` : close;
      alerts.push(infobar('ok', flash.title, esc(flash.text), extra));
    } else alerts.push(infobar('bad', flash.title, esc(flash.text), close));
  }
  if (g.missing) alerts.push(infobar('bad', t('st_missing'), esc(t('w_missing'))));
  else if (!g.exe) alerts.push(infobar('warn', t('w_noexe_t'), esc(t('w_noexe'))));
  if (g.bitsMismatch) alerts.push(infobar('warn', t('w_bits_t'), esc(t('w_bits', { exe: g.exe, b: g.exeBits, pb: p.bits, other: p.bits === 64 ? 'i686' : 'x86_64' }))));
  if (g.status.foreign && !installed) alerts.push(infobar('warn', t('w_foreign_t'), esc(t('w_foreign', { file: g.status.foreign }))));

  // route cards
  const modes = ROUTES.map((r) => {
    const av = routes[r] ? routes[r].available : false;
    const reason = routes[r] && routes[r].reason === 'only64';
    const isCur = installed && g.status.route === r;
    return `<button class="mode ${f.route === r ? 'sel' : ''}" data-act="route" data-route="${r}" ${av ? '' : 'disabled'} aria-pressed="${f.route === r}">
      <div class="ic">${icon(ROUTE_ICON[r], 'lg')}</div>
      <div style="min-width:0"><div class="t"><span>${esc(routeName(r))}</span>${isCur ? `<span class="badge">${esc(t('current'))}</span>` : ''}</div>
        <div class="d">${esc(reason ? t('only64') : t('rd_' + ROUTE_KEY[r]))}</div></div></button>`;
  }).join('');

  const opti = f.route === 'optiscaler';
  const wantNr = !opti || !hasOpenRr || f.nr;
  const wantOpenRr = opti && hasOpenRr && f.openrr;
  const wantOptiPatcher = opti && hasOptiPatcher && f.optipatcher;

  // ASI plugins section (OptiScaler route on x86_64 when OpenRR is bundled)
  let pluginsSec = '';
  if (opti && hasOpenRr) {
    const dlssdBadge = g.hasNativeDlssd
      ? `<span class="mstat ok" style="margin-left:8px">${icon('checkCircle')}${esc(t('p_dlssd_yes'))}</span>`
      : `<span class="mstat missing" style="margin-left:8px">${icon('info')}${esc(t('p_dlssd_no'))}</span>`;
    let dlssdRow = '';
    if (wantOpenRr) {
      const hasCached = !!dlssdInfo.cached;
      const dTitle = hasCached ? t('dlssd_ok') : g.hasNativeDlssd ? t('dlssd_game') : t('dlssd_need');
      const dDesc = hasCached ? t('dlssd_ok_d') : g.hasNativeDlssd ? t('dlssd_game_d') : t('dlssd_need_d');
      const dIcon = hasCached ? 'checkCircle' : g.hasNativeDlssd ? 'info' : 'warn';
      const dColor = hasCached ? 'var(--success)' : g.hasNativeDlssd ? 'var(--muted)' : 'var(--warning)';
      const showDInput = !hasCached || f.showDlssd;
      dlssdRow = `
        <div class="row">${icon('shield')}<div><div class="t">${esc(t('p_spoof'))}</div><div class="d">${esc(t('p_spoof_d'))}</div></div>
          <div class="ctl"><button class="switch" role="switch" aria-checked="${f.openrrSpoof === 'auto'}" data-act="openrr-spoof"><span class="lbl">${esc(f.openrrSpoof === 'auto' ? t('on') : t('off'))}</span><span class="track"><span class="knob"></span></span></button></div></div>
        <div class="row ${showDInput ? 'stack' : ''}"><span style="color:${dColor}">${icon(dIcon, 'lg')}</span>
          <div><div class="t">${esc(dTitle)}</div><div class="d">${esc(dDesc)}</div></div>
          ${showDInput ? `<div class="full"><input class="input mono" id="dlssd" spellcheck="false" placeholder="${esc(t('dlssd_ph'))}" value="${esc(f.dlssd)}">
            <button class="btn" data-act="browse-dlssd">${icon('folder', 'sm')}${esc(t('browse'))}</button>
            ${hasCached ? `<button class="btn subtle" data-act="toggle-dlssd">${esc(t('hide'))}</button>` : ''}</div>`
          : `<div class="ctl"><button class="btn subtle sm" data-act="toggle-dlssd">${esc(t('other_dlssd'))}</button></div>`}
        </div>`;
    }
    const patcherRow = hasOptiPatcher ? `
      <div class="row">${icon('wand')}<div><div class="t">${esc(t('p_optipatcher'))}</div><div class="d">${esc(t('p_optipatcher_d'))}</div></div>
        <div class="ctl"><button class="switch" role="switch" aria-checked="${wantOptiPatcher}" data-act="optipatcher"><span class="lbl">${esc(wantOptiPatcher ? t('on') : t('off'))}</span><span class="track"><span class="knob"></span></span></button></div></div>` : '';
    pluginsSec = `<section class="section"><div class="section-head"><h3>${esc(t('sec_plugins'))}</h3></div>
      <div class="card">
        <div class="row">${icon('chip')}<div><div class="t">${esc(t('p_nr'))}</div><div class="d">${esc(t('p_nr_d'))}</div></div>
          <div class="ctl"><button class="switch" role="switch" aria-checked="${f.nr}" data-act="toggle-nr"><span class="lbl">${esc(f.nr ? t('on') : t('off'))}</span><span class="track"><span class="knob"></span></span></button></div></div>
        <div class="row">${icon('layers')}<div><div class="t">${esc(t('p_openrr', { ver: p.openrr.version || '0.1.7plus3' }))}${dlssdBadge}</div><div class="d">${esc(t('p_openrr_d'))}</div></div>
          <div class="ctl"><button class="switch" role="switch" aria-checked="${f.openrr}" data-act="toggle-openrr"><span class="lbl">${esc(f.openrr ? t('on') : t('off'))}</span><span class="track"><span class="knob"></span></span></button></div></div>
        ${dlssdRow}
        ${patcherRow}
      </div></section>`;
  }

  // options (NR options; dimmed when NR is disabled on OptiScaler route)
  const options = `
    <div class="card">
      <div class="row ${wantNr ? '' : 'dis'}">${icon('chip')}<div><div class="t">${esc(t('o_model'))}</div><div class="d">${esc(t('o_model_d'))}</div></div>
        <div class="ctl"><select class="select" id="sel-model" aria-label="${esc(t('o_model'))}" ${wantNr ? '' : 'disabled'}>${MODELS.map((m) => `<option value="${m}" ${f.model === m ? 'selected' : ''}>${esc(t('m_' + m))}</option>`).join('')}</select></div></div>
      <div class="row ${wantNr ? '' : 'dis'}">${icon('wand')}<div><div class="t">${esc(t('o_style'))}</div><div class="d">${esc(t('o_style_d'))}</div></div>
        <div class="ctl"><div class="seg" role="radiogroup">${['renodx', 'wilsjo2'].map((s) => `<button role="radio" aria-checked="${f.style === s}" class="${f.style === s ? 'on' : ''}" data-act="style" data-style="${s}" ${wantNr ? '' : 'disabled'}>${s === 'renodx' ? 'RenoDX' : 'wilsjo2'}</button>`).join('')}</div></div></div>
      <div class="row ${opti ? 'dis' : ''}">${icon('motion')}<div><div class="t">${esc(t('o_lum'))}</div><div class="d">${esc(opti ? t('o_lum_opti') : t('o_lum_d'))}</div></div>
        <div class="ctl"><button class="switch" role="switch" aria-checked="${!opti && f.lumenite}" data-act="lumenite" ${opti ? 'disabled' : ''}><span class="lbl">${esc(!opti && f.lumenite ? t('on') : t('off'))}</span><span class="track"><span class="knob"></span></span></button></div></div>
    </div>`;

  // model (Mochizuki NR model; only required when NR is enabled)
  let mTitle, mDesc, mIcon, needDll;
  if (model.complete) { mTitle = t('mdl_ok'); mDesc = t('mdl_ok_d'); mIcon = 'checkCircle'; needDll = false; }
  else if (model.ready) { mTitle = t('mdl_part'); mDesc = t('mdl_part_d'); mIcon = 'info'; needDll = false; }
  else if (model.aardwold === 'bad' || model.small === 'bad') { mTitle = t('mdl_bad'); mDesc = t('mdl_bad_d'); mIcon = 'error'; needDll = true; }
  else { mTitle = t('mdl_need'); mDesc = t('mdl_need_d'); mIcon = 'warn'; needDll = true; }
  const showInput = needDll || f.showDll;
  const mColor = model.complete ? 'var(--success)' : needDll ? (mIcon === 'error' ? 'var(--danger)' : 'var(--warning)') : 'var(--muted)';
  const modelSec = wantNr ? `<section class="section"><div class="section-head"><h3>${esc(t('sec_model'))}</h3></div>
    <div class="card"><div class="row ${showInput ? 'stack' : ''}"><span style="color:${mColor}">${icon(mIcon, 'lg')}</span>
      <div><div class="t">${esc(mTitle)}</div><div class="d">${esc(mDesc)}</div></div>
      ${showInput ? `<div class="full"><input class="input mono" id="dll" spellcheck="false" placeholder="${esc(t('dll_ph'))}" value="${esc(f.dll)}">
        <button class="btn" data-act="browse-dll">${icon('folder', 'sm')}${esc(t('browse'))}</button>
        ${needDll ? '' : `<button class="btn subtle" data-act="toggle-dll">${esc(t('hide'))}</button>`}</div>`
      : `<div class="ctl"><button class="btn subtle sm" data-act="toggle-dll">${esc(t('other_dll'))}</button></div>`}
    </div></div></section>` : '';

  // launch options
  let launchSec = '';
  if (installed) {
    const rec = g.record || {};
    const main = rec.launch || inferredLaunch(g.status.route);
    if (main) {
      const block = (label, val, guess) => `<div class="launch-block"><div class="lbl">${esc(label)}${guess ? ` <span class="tag">${esc(t('launch_guess'))}</span>` : ''}</div>
        <div class="code"><code>${esc(val)}</code><button class="btn sm" data-act="copy" data-text="${esc(val)}">${icon('copy', 'sm')}${esc(t('copy'))}</button></div></div>`;
      launchSec = `<section class="section"><div class="section-head"><h3>${esc(t('sec_launch'))}</h3><span class="aside">${esc(t('launch_hint'))}</span></div>
        <div class="card">${block(t('launch_main'), main, !rec.launch)}${rec.launchAlt ? block(t('launch_alt'), rec.launchAlt, false) : ''}</div></section>`;
    }
  }

  // console
  const lines = jobHere ? S.jobLines : [];
  const jstate = jobHere ? S.job.state : null;
  const jlabel = jobHere ? (jstate === 'running' ? t('j_' + S.job.kind) : t(jstate === 'done' ? 'j_done' : 'j_failed')) : '';
  const consoleSec = `<section class="section"><div class="card">
      <div class="console-head"><span class="t">${esc(t('sec_output'))}</span>
        <span class="s">${jstate === 'running' ? '<span class="spinner"></span>' : jstate === 'done' ? `<span style="color:var(--success)">${icon('check', 'sm')}</span>` : jstate === 'failed' ? `<span style="color:var(--danger)">${icon('error', 'sm')}</span>` : ''}${esc(jlabel)}</span>
        <span class="grow"></span>${lines.length && jstate !== 'running' ? `<button class="btn subtle sm" data-act="clear-log">${esc(t('clear'))}</button>` : ''}</div>
      <pre class="console" id="console">${lines.length ? lines.map(lineHtml).join('') : `<span class="empty">${esc(t('out_idle'))}</span>`}</pre></div></section>`;

  // action bar
  const noPlugin = opti && !wantNr && !wantOpenRr;
  const missingNrDll = wantNr && needDll && !f.dll.trim();
  const canInstall = p.ok && !g.missing && !running && (routes[f.route] && routes[f.route].available) && !noPlugin && !missingNrDll;
  let summary;
  if (running) summary = esc(t('sum_busy'));
  else if (noPlugin) summary = esc(t('sum_need_plugin'));
  else if (missingNrDll) summary = esc(t('sum_need_model'));
  else if (installed) summary = tv('sum_re', { route: routeName(g.status.route || f.route) });
  else summary = tv('sum_install', { route: routeName(f.route) });
  const primaryBusy = running && jobHere && S.job.kind === 'install';

  host.innerHTML = `
    <div class="detail-inner">
      <div class="hero">
        <div class="hero-art">${heroBg}<div class="fade"></div>
          <div class="hero-body"><div class="hero-meta"><span class="tag">${esc(t('src_' + g.source))}</span>${bitsTag}${g.override ? `<span class="tag">${esc(t('override_on'))}</span>` : ''}</div>
            <h1 title="${esc(g.name)}">${esc(g.name)}</h1><div class="state">${stateLine}</div></div></div>
        <div class="pathbar">${icon('folder')}<span class="path" title="${esc(g.folder)}">${esc(g.folder)}</span>
          ${g.override ? `<button class="btn subtle sm" data-act="reset-folder">${esc(t('reset'))}</button>` : ''}
          ${g.source === 'manual' ? `<button class="btn subtle sm" data-act="remove-manual">${esc(t('remove_entry'))}</button>` : ''}
          <button class="btn subtle sm" data-act="change-folder">${esc(t('change'))}</button>
          <button class="btn sm" data-act="open-folder" ${g.missing ? 'disabled' : ''}>${icon('folder', 'sm')}${esc(t('openFolder'))}</button></div>
      </div>
      ${alerts.join('')}
      ${launchSec}
      <section class="section"><div class="section-head"><h3>${esc(t('sec_route'))}</h3></div><div class="modes">${modes}</div></section>
      ${pluginsSec}
      <section class="section"><div class="section-head"><h3>${esc(t('sec_options'))}</h3></div>${options}</section>
      ${modelSec}
      ${consoleSec}
    </div>
    <div class="actionbar"><div class="actionbar-inner">
      <div class="summary">${summary}</div>
      ${installed ? `<button class="btn danger-text" data-act="uninstall" ${running ? 'disabled' : ''}>${icon('trash', 'sm')}${esc(t('uninstall'))}</button><span class="sep"></span>` : ''}
      <button class="btn" data-act="logs" ${running || g.missing || !p.ok ? 'disabled' : ''}>${icon('box', 'sm')}${esc(t('packLogs'))}</button>
      <button class="btn primary" data-act="install" ${canInstall ? '' : 'disabled'}>${primaryBusy ? '<span class="spinner"></span>' : icon('download', 'sm')}${esc(installed ? t('reinstall') : t('install'))}</button>
    </div></div>`;
  host.scrollTop = scrollTop;
  scrollConsole();
}

function lineHtml(l) {
  const cls = l.s === 'err' ? 'err' : l.s === 'sys' ? 'sys' : /^(Installed the|Steam launch options|Uninstalled from|Logs packed|OpenRR |OptiPatcher)/.test(l.t) ? 'hl' : '';
  return `<span class="${cls}">${esc(l.t)}</span>\n`;
}
function scrollConsole() { const c = $('#console'); if (c) c.scrollTop = c.scrollHeight; }

function renderStatus() {
  const el = $('#status-left');
  if (!el) return;
  const p = pkg();
  if (busy()) {
    el.innerHTML = `<span class="spinner"></span><span>${esc(t('j_' + S.job.kind))} · ${esc(S.job.meta.name || '')}</span>`;
  } else if (!S.connected) {
    el.innerHTML = `<span style="color:var(--warning)">${icon('warn', 'sm')}</span><span>${esc(t('lost'))}</span>`;
  } else if (!S.loaded) {
    el.innerHTML = `<span class="spinner"></span><span>${esc(t('scanning'))}</span>`;
  } else {
    const inst = S.games.filter((g) => g.status.installed).length;
    const orr = p.openrr && p.openrr.available ? ` · OpenRR ${p.openrr.version || '0.1.7plus3'}` : '';
    el.innerHTML = `<span class="dot ok" style="opacity:${p.ok ? 1 : 0}"></span><span>${esc(t('ready'))} · ${esc(t('games_n', { n: S.games.length }))}${inst ? ` · ${esc(t('inst_n', { n: inst }))}` : ''}${p.upstreamVersion ? ` · DLSSNR-AMD ${esc(p.upstreamVersion)}` : ''}${esc(orr)}</span>`;
  }
}

/* --------------------------------------------------------------------------------------- dialogs */
function openDialog(d) { S.dialog = d; renderDialog(); }
function closeDialog() { S.dialog = null; renderDialog(); }
function renderDialog() {
  const root = $('#overlay-root');
  const d = S.dialog;
  if (!d) { root.innerHTML = ''; root._dlg = null; return; }
  // Build the overlay + dialog shell only when a dialog opens, so the open animation plays once;
  // later refreshes (live info, browse results, language/theme switches) just swap the contents.
  let box = root.querySelector('.dialog');
  const fresh = !box || root._dlg !== d;
  if (fresh) {
    root.innerHTML = `<div class="overlay" data-act="backdrop"><div class="dialog${d.type === 'browse' ? ' wide' : ''}" role="dialog" aria-modal="true" tabindex="-1"></div></div>`;
    box = root.querySelector('.dialog');
    root._dlg = d;
  }
  const scrollers = fresh ? [] : [...box.querySelectorAll('.dialog-body, .fb-list')].map((el) => el.scrollTop);
  if (d.type === 'confirm') {
    box.innerHTML = `<div class="dialog-head"><h2>${esc(d.title)}</h2></div>
      <div class="dialog-body"><p>${esc(d.text)}</p></div>
      <div class="dialog-foot"><button class="btn" data-act="dlg-cancel">${esc(t('cancel'))}</button><button class="btn danger" data-act="dlg-ok">${esc(d.ok)}</button></div>`;
  } else if (d.type === 'settings') {
    box.innerHTML = settingsHtml();
  } else if (d.type === 'browse') {
    box.innerHTML = browseHtml(d);
  }
  if (scrollers.length) box.querySelectorAll('.dialog-body, .fb-list').forEach((el, i) => { el.scrollTop = scrollers[i] || 0; });
  // Focus the dialog itself (keeps keyboard Tab/Esc inside it without a heavy ring on a button).
  if (fresh) setTimeout(() => { if (root._dlg === d) box.focus({ preventScroll: true }); }, 0);
}

function settingsHtml() {
  const info = S.info || {};
  const p = pkg();
  const m = info.model || {};
  const dl = info.dlssd || {};
  const hasOpenRr = !!(p.openrr && p.openrr.available);
  const mc = (k) => `<span class="mstat ${m[k] || 'missing'}">${icon(m[k] === 'ok' ? 'checkCircle' : m[k] === 'bad' ? 'error' : 'info')}${k === 'aardwold' ? 'CC_SILVER_AARDWOLD' : 'CC_SMALL_FP8'} · ${esc(t('mc_' + (m[k] || 'missing')))}</span>`;
  const dc = hasOpenRr ? `<span class="mstat ${dl.cached ? 'ok' : 'missing'}">${icon(dl.cached ? 'checkCircle' : 'info')}nvngx_dlssd.dll · ${esc(t(dl.cached ? 'mc_ok' : 'mc_missing'))}</span>` : '';
  const seg = (act, cur, items) => `<div class="seg">${items.map(([v, l]) => `<button class="${cur === v ? 'on' : ''}" data-act="${act}" data-v="${v}">${esc(l)}</button>`).join('')}</div>`;
  return `<div class="dialog-head"><h2>${esc(t('settings'))}</h2><button class="icon-btn sm" data-act="dlg-cancel" aria-label="${esc(t('close'))}">${icon('x')}</button></div>
    <div class="dialog-body">
      <div class="settings-group"><div class="lbl">${esc(t('set_appearance'))}</div><div class="card">
        <div class="row">${icon('globe')}<div class="t">${esc(t('set_lang'))}</div><div class="ctl">${seg('set-lang', S.lang, [['zh', '中文'], ['en', 'English']])}</div></div>
        <div class="row">${icon('theme')}<div class="t">${esc(t('set_theme'))}</div><div class="ctl">${seg('set-theme', S.themePref, [['light', t('light')], ['dark', t('dark')], ['system', t('system')]])}</div></div>
      </div></div>
      <div class="settings-group"><div class="lbl">${esc(t('set_pkg'))}</div><div class="card"><dl class="kv">
        <dt>${esc(t('set_ver'))}</dt><dd>${esc(p.version || '—')}</dd>
        <dt>${esc(t('set_arch'))}</dt><dd>Linux ${esc(p.arch)} · ${esc(t('bits', { b: p.bits }))}</dd>
        <dt>${esc(t('set_up'))}</dt><dd>${esc(p.upstreamVersion || '—')}</dd>
        ${hasOpenRr ? `<dt>${esc(t('set_openrr'))}</dt><dd>${esc(p.openrr.version || '0.1.7plus3')}${p.optipatcher ? ' · OptiPatcher' : ''}</dd>` : ''}
        <dt>${esc(t('set_path'))}</dt><dd title="${esc(p.root || '')}">${esc(p.root || '—')}</dd>
        <dt>${esc(t('set_model'))}</dt><dd class="wrap">${mc('small')}${mc('aardwold')}${dc}</dd>
      </dl></div></div>
      <div class="settings-group"><div class="lbl">${esc(t('set_int'))}</div><div class="card">
        <div class="row">${icon('menu')}<div><div class="t">${esc(t('set_desktop'))}</div><div class="d">${esc(t('set_desktop_d'))}</div></div>
          <div class="ctl">${info.desktopInstalled ? `<button class="btn sm" data-act="desktop-remove">${esc(t('remove'))}</button>` : `<button class="btn sm" data-act="desktop-add" ${p.ok ? '' : 'disabled'}>${esc(t('add'))}</button>`}</div></div>
        <div class="row">${icon('folder')}<div><div class="t">${esc(t('set_pkg'))}</div><div class="d">${esc(p.root || '')}</div></div>
          <div class="ctl"><button class="btn sm" data-act="open-package" ${p.ok ? '' : 'disabled'}>${esc(t('open'))}</button></div></div>
        <div class="row">${icon('power')}<div><div class="t">${esc(t('set_quit'))}</div><div class="d">${esc(t('set_quit_d'))}</div></div>
          <div class="ctl"><button class="btn sm" data-act="quit" ${busy() ? 'disabled' : ''}>${esc(t('quit'))}</button></div></div>
      </div></div>
    </div>
    <div class="dialog-foot"><span class="grow fb-foot-path">${esc(t('notice'))}</span><button class="btn" data-act="dlg-cancel">${esc(t('close'))}</button></div>`;
}

function browseHtml(d) {
  const r = d.res;
  const isFile = d.mode === 'model' || d.mode === 'dlssd';
  const titleKey = d.mode === 'dlssd' ? 'fb_dlssd' : d.mode === 'model' ? 'fb_model' : 'fb_dir';
  const placeIcon = { home: 'home', downloads: 'download', desktop: 'monitor', documents: 'file', steam: 'gamepad', drive: 'drive' };
  const places = (r ? r.places : []).map((pl) => `<button class="fb-place" data-act="fb-go" data-path="${esc(pl.path)}" title="${esc(pl.path)}">${icon(placeIcon[pl.key] || 'folder')}<span>${esc(pl.key === 'steam' || pl.key === 'drive' ? (pl.path.split('/').filter(Boolean).slice(-3).join('/')) : t('pl_' + pl.key))}</span></button>`).join('');
  let rows = '';
  if (!r) rows = '<div class="fb-empty"><span class="spinner" style="display:inline-block"></span></div>';
  else if (r.error) rows = `<div class="fb-empty">${esc(r.error)}</div>`;
  else if (!r.entries.length) rows = `<div class="fb-empty">${esc(t(isFile ? 'fb_noitems' : 'fb_empty'))}</div>`;
  else rows = r.entries.map((e) => `<button class="fb-row ${e.dir ? 'dir' : ''} ${d.sel === e.name ? 'sel' : ''} ${!e.dir && d.mode === 'dir' ? 'muted' : ''}" data-act="fb-item" data-name="${esc(e.name)}" data-dir="${e.dir ? 1 : 0}">${icon(e.dir ? 'folder' : e.exe ? 'exe' : 'file')}<span class="nm">${esc(e.name)}</span><span class="sz">${e.dir ? '' : esc(fmtSize(e.size))}</span></button>`).join('');
  const canPick = d.mode === 'dir' ? !!r : !!d.sel;
  return `<div class="dialog-head"><h2>${esc(t(titleKey))}</h2><button class="icon-btn sm" data-act="dlg-cancel" aria-label="${esc(t('close'))}">${icon('x')}</button></div>
    <div class="fb"><div class="fb-places">${places}</div>
      <div class="fb-main"><div class="fb-bar"><button class="btn sm" data-act="fb-up" ${r && r.parent ? '' : 'disabled'} title="${esc(t('fb_up'))}">${icon('up', 'sm')}</button>
        <input class="input mono" id="fb-path" spellcheck="false" value="${esc(r ? r.path : d.path || '')}">
        <button class="btn sm" data-act="fb-path-go">${esc(t('fb_go'))}</button></div>
        <div class="fb-list" id="fb-list">${rows}</div></div></div>
    <div class="dialog-foot"><label class="check"><input type="checkbox" id="fb-hidden" ${d.hidden ? 'checked' : ''}>${esc(t('fb_hidden'))}</label><span class="grow"></span>
      <button class="btn" data-act="dlg-cancel">${esc(t('cancel'))}</button>
      <button class="btn primary" data-act="fb-pick" ${canPick ? '' : 'disabled'}>${esc(t(d.mode === 'dir' ? 'fb_selDir' : 'fb_select'))}</button></div>`;
}

async function browse(d, path) {
  d.res = null; d.sel = null; renderDialog();
  try {
    d.res = await api(`/api/browse?mode=${d.mode}&hidden=${d.hidden ? 1 : 0}&path=${encodeURIComponent(path || '')}`);
  } catch (e) { d.res = { path: path || '', entries: [], places: [], error: errText(e) }; }
  if (S.dialog === d) renderDialog();
}
function openBrowser(mode, start, onPick) {
  const d = { type: 'browse', mode, hidden: false, res: null, sel: null, onPick };
  openDialog(d);
  browse(d, start);
}

/* ------------------------------------------------------------------------------------------ toast */
let toastTimer = null;
function toast(text, bad) {
  const root = $('#toast-root');
  root.innerHTML = `<div class="toast ${bad ? 'bad' : ''}">${icon(bad ? 'error' : 'checkCircle')}<span>${esc(text)}</span></div>`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { root.innerHTML = ''; }, 2600);
}

/* ------------------------------------------------------------------------------------------- data */
async function loadInfo() {
  S.info = await api('/api/info');
  const s = S.info.settings || {};
  S.lang = OPT.lang || s.lang || S.info.defaultLang || 'en';
  S.themePref = OPT.theme || s.theme || 'system';
  applyTheme();
}
async function loadGames(refresh) {
  S.scanning = true; const b = $('#btn-rescan'); if (b) b.classList.add('spin');
  try {
    const r = await api('/api/games' + (refresh ? '?refresh=1' : ''));
    S.games = r.games; S.loaded = true;
    if (!S.selId || !S.games.some((g) => g.id === S.selId)) {
      const want = OPT.select && S.games.find((g) => g.id === OPT.select || g.name === OPT.select);
      S.selId = want ? want.id : (S.games[0] ? S.games[0].id : null);
      OPT.select = null;
    }
  } finally {
    S.scanning = false; const b2 = $('#btn-rescan'); if (b2) b2.classList.remove('spin');
  }
  renderLibrary(); renderDetail(); renderStatus();
}
async function refreshGame(id) {
  try {
    const r = await api('/api/game?refresh=1&id=' + encodeURIComponent(id));
    const i = S.games.findIndex((g) => g.id === id);
    if (i >= 0) S.games[i] = r.game;
  } catch (e) { /* keep the old entry */ }
}
async function saveSettings(patch) { try { await api('/api/settings', patch); } catch (e) { /* not fatal */ } }

/* ------------------------------------------------------------------------------------------ jobs */
async function startJob(path, body) {
  try {
    const r = await api(path, body);
    onJob(r.job, true);
  } catch (e) { toast(errText(e), true); }
}
function onJob(job, fresh) {
  if (!job) return;
  const isNew = !S.job || S.job.id !== job.id;
  if (isNew || job.since === 0) { S.jobLines = job.lines ? job.lines.slice() : []; }
  const prev = S.job;
  S.job = Object.assign({}, job, { lines: undefined });
  if (S.firstJobEvent && !fresh && job.state !== 'running') S.handled.add(job.id); // finished before this page loaded
  S.firstJobEvent = false;
  if (job.state !== 'running' && !S.handled.has(job.id)) { S.handled.add(job.id); finished(job); }
  if (isNew || !prev || prev.state !== job.state) { renderDetail(); renderStatus(); }
}
function appendLines(lines) {
  for (const l of lines) S.jobLines.push(l);
  const c = $('#console');
  const g = selected();
  if (c && g && S.job && S.job.meta && S.job.meta.gameId === g.id) {
    if (c.querySelector('.empty')) c.innerHTML = '';
    c.insertAdjacentHTML('beforeend', lines.map(lineHtml).join(''));
    scrollConsole();
  }
}
async function finished(job) {
  const id = job.meta && job.meta.gameId;
  const r = job.result || {};
  const ok = job.state === 'done';
  if (id) {
    let title, text;
    if (!ok) { title = t('failed_t'); text = r.error || ''; }
    else if (job.kind === 'install') { title = t('ok_install', { route: routeName(job.meta.route) }); text = t('ok_install_d'); }
    else if (job.kind === 'remove') { title = t('ok_remove'); text = t('ok_remove_d'); }
    else { title = t('ok_logs'); text = r.logs || ''; }
    S.flash[id] = { ok, title, text, logs: job.kind === 'logs' ? r.logs : null };
    if (job.kind === 'install' && ok && S.forms[id]) { S.forms[id].dll = ''; S.forms[id].dlssd = ''; }
    toast(ok ? title : `${t('failed_t')}: ${(r.error || '').split('\n').pop()}`, !ok);
  }
  try { S.info = await api('/api/info'); } catch (e) { /* ignore */ }
  if (id) await refreshGame(id);
  renderLibrary(); renderDetail(); renderStatus();
}

let es = null, esFails = 0;
function connectEvents() {
  if (OPT.noevents || !window.EventSource) return;
  es = new EventSource('/api/events?t=' + encodeURIComponent(TOKEN));
  es.onmessage = (ev) => {
    esFails = 0;
    if (!S.connected) { S.connected = true; renderStatus(); }
    let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
    if (m.type === 'job') onJob(m.job, false);
    else if (m.type === 'lines' && S.job && m.id === S.job.id) appendLines(m.lines);
  };
  es.onerror = () => {
    esFails++;
    if (esFails >= 2 && S.connected) { S.connected = false; renderStatus(); }
    if (esFails >= 8) { es.close(); stopped(); }
  };
}
function stopped() {
  if (S.stopped) return;
  S.stopped = true;
  document.body.insertAdjacentHTML('beforeend', `<div class="stopped"><div>${LOGO.replace('class="logo"', 'class="logo" style="width:48px;height:48px"')}<h2>${esc(t('stopped'))}</h2><div>${esc(t('stopped_d'))}</div></div></div>`);
  try { window.close(); } catch (e) { /* browsers may refuse */ }
}

/* ---------------------------------------------------------------------------------------- actions */
async function copyText(text) {
  try { await navigator.clipboard.writeText(text); }
  catch (e) {
    const ta = document.createElement('textarea'); ta.value = text; document.body.appendChild(ta); ta.select();
    try { document.execCommand('copy'); } finally { ta.remove(); }
  }
  toast(t('copied'));
}

const ACT = {
  async select(el) { S.selId = el.dataset.id; renderLibrary(); renderDetail(); $('#detail').scrollTop = 0; },
  async rescan() { await loadGames(true); },
  async lang() { S.lang = S.lang === 'zh' ? 'en' : 'zh'; saveSettings({ lang: S.lang }); renderShell(); renderDialog(); },
  async theme() { const order = ['light', 'dark', 'system']; S.themePref = order[(order.indexOf(S.themePref) + 1) % 3]; applyTheme(); saveSettings({ theme: S.themePref }); renderShell(); toast(`${t('theme')}: ${t(S.themePref)}`); },
  async settings() { try { S.info = await api('/api/info'); } catch (e) { /* stale ok */ } openDialog({ type: 'settings' }); },
  async 'set-lang'(el) { S.lang = el.dataset.v; saveSettings({ lang: S.lang }); renderShell(); renderDialog(); },
  async 'set-theme'(el) { S.themePref = el.dataset.v; applyTheme(); saveSettings({ theme: S.themePref }); renderShell(); renderDialog(); },
  async route(el) { const g = selected(); formFor(g).route = el.dataset.route; renderDetail(); },
  async style(el) { const g = selected(); formFor(g).style = el.dataset.style; renderDetail(); },
  async lumenite() { const g = selected(); const f = formFor(g); f.lumenite = !f.lumenite; renderDetail(); },
  async 'toggle-nr'() { const g = selected(); const f = formFor(g); f.nr = !f.nr; renderDetail(); },
  async 'toggle-openrr'() { const g = selected(); const f = formFor(g); f.openrr = !f.openrr; renderDetail(); },
  async 'openrr-spoof'() { const g = selected(); const f = formFor(g); f.openrrSpoof = f.openrrSpoof === 'auto' ? 'full' : 'auto'; renderDetail(); },
  async optipatcher() { const g = selected(); const f = formFor(g); f.optipatcher = !f.optipatcher; renderDetail(); },
  async 'toggle-dll'() { const g = selected(); const f = formFor(g); f.showDll = !f.showDll; renderDetail(); if (f.showDll) { const i = $('#dll'); if (i) i.focus(); } },
  async 'toggle-dlssd'() { const g = selected(); const f = formFor(g); f.showDlssd = !f.showDlssd; renderDetail(); if (f.showDlssd) { const i = $('#dlssd'); if (i) i.focus(); } },
  async 'browse-dll'() {
    const g = selected(); const f = formFor(g);
    openBrowser('model', f.dll || (S.info && S.info.home + '/Downloads'), (path) => { f.dll = path; renderDetail(); });
  },
  async 'browse-dlssd'() {
    const g = selected(); const f = formFor(g);
    openBrowser('dlssd', f.dlssd || (S.info && S.info.home + '/Downloads'), (path) => { f.dlssd = path; renderDetail(); });
  },
  async install() {
    const g = selected(); const f = formFor(g);
    const p = pkg();
    const opti = f.route === 'optiscaler';
    const hasOpenRr = !!(p.openrr && p.openrr.available);
    const wantNr = !opti || !hasOpenRr || f.nr;
    const wantOpenRr = opti && hasOpenRr && f.openrr;
    const wantOptiPatcher = opti && !!p.optipatcher && f.optipatcher;
    startJob('/api/install', {
      id: g.id, route: f.route, model: f.model, style: f.style,
      lumenite: !opti && f.lumenite,
      nr: wantNr, openrr: wantOpenRr, openrrSpoof: f.openrrSpoof, optipatcher: wantOptiPatcher,
      dll: wantNr ? (f.dll.trim() || null) : null,
      dlssd: wantOpenRr ? (f.dlssd.trim() || null) : null,
    });
    delete S.flash[g.id];
  },
  async uninstall() {
    const g = selected();
    openDialog({ type: 'confirm', title: t('cf_title', { name: g.name }), text: t('cf_text'), ok: t('uninstall'),
      onOk: () => { delete S.flash[g.id]; startJob('/api/uninstall', { id: g.id }); } });
  },
  async logs() { const g = selected(); delete S.flash[g.id]; startJob('/api/logs', { id: g.id }); },
  async 'open-folder'() { const g = selected(); try { await api('/api/open', { id: g.id }); } catch (e) { toast(errText(e), true); } },
  async 'open-logs'(el) { try { await api('/api/open', { target: 'logs', path: el.dataset.path }); } catch (e) { toast(errText(e), true); } },
  async 'open-package'() { try { await api('/api/open', { target: 'package' }); } catch (e) { toast(errText(e), true); } },
  async copy(el) { copyText(el.dataset.text); },
  async dismiss() { const g = selected(); delete S.flash[g.id]; renderDetail(); },
  async 'clear-log'() { S.jobLines = []; S.job = null; renderDetail(); },
  async 'add-game'() {
    openBrowser('dir', '', async (path) => {
      try {
        const r = await api('/api/library/add', { path });
        await loadGames(false);
        if (r.id) { S.selId = r.id; renderLibrary(); renderDetail(); }
        toast(t('manualAdded'));
      } catch (e) { toast(errText(e), true); }
    });
  },
  async 'change-folder'() {
    const g = selected();
    openBrowser('dir', g.folder, async (path) => {
      try { await api('/api/library/override', { id: g.id, folder: path }); delete S.forms[g.id]; await loadGames(false); }
      catch (e) { toast(errText(e), true); }
    });
  },
  async 'reset-folder'() { const g = selected(); try { await api('/api/library/override', { id: g.id, folder: null }); await loadGames(false); } catch (e) { toast(errText(e), true); } },
  async 'remove-manual'() { const g = selected(); try { await api('/api/library/remove', { id: g.id }); S.selId = null; await loadGames(false); } catch (e) { toast(errText(e), true); } },
  async 'desktop-add'() { try { await api('/api/desktop', { action: 'install' }); S.info = await api('/api/info'); renderDialog(); toast(t('desktopAdded')); } catch (e) { toast(errText(e), true); } },
  async 'desktop-remove'() { try { await api('/api/desktop', { action: 'remove' }); S.info = await api('/api/info'); renderDialog(); toast(t('desktopRemoved')); } catch (e) { toast(errText(e), true); } },
  async quit() { try { await api('/api/quit', {}); } catch (e) { if (e.code === 'busy') { toast(errText(e), true); return; } } closeDialog(); if (es) es.close(); stopped(); },
  async backdrop(el, ev) { if (ev.target === el) closeDialog(); },
  async 'dlg-cancel'() { closeDialog(); },
  async 'dlg-ok'() { const d = S.dialog; closeDialog(); if (d && d.onOk) d.onOk(); },
  async 'fb-go'(el) { browse(S.dialog, el.dataset.path); },
  async 'fb-up'() { const d = S.dialog; if (d.res && d.res.parent) browse(d, d.res.parent); },
  async 'fb-path-go'() { browse(S.dialog, $('#fb-path').value.trim()); },
  async 'fb-item'(el, ev) {
    const d = S.dialog; const name = el.dataset.name; const isDir = el.dataset.dir === '1';
    const full = d.res.path.replace(/\/$/, '') + '/' + name;
    if (isDir) { browse(d, full); return; }
    if (d.mode === 'model' || d.mode === 'dlssd') {
      if (ev.detail >= 2) { closeDialog(); d.onPick(full); return; }
      d.sel = name; renderDialog();
    }
  },
  async 'fb-pick'() {
    const d = S.dialog; if (!d || !d.res) return;
    const path = d.mode === 'dir' ? d.res.path : d.res.path.replace(/\/$/, '') + '/' + d.sel;
    closeDialog(); d.onPick(path);
  },
};

document.addEventListener('click', (ev) => {
  const el = ev.target.closest('[data-act]');
  if (!el || el.disabled) return;
  const fn = ACT[el.dataset.act];
  if (fn) { if (el.dataset.act !== 'backdrop') ev.stopPropagation(); fn(el, ev); }
});
document.addEventListener('input', (ev) => {
  const id = ev.target.id;
  if (id === 'search') { S.search = ev.target.value; renderLibrary(); }
  else if (id === 'dlssd') {
    const g = selected(); if (g) formFor(g).dlssd = ev.target.value;
  }
  else if (id === 'dll') {
    const g = selected(); formFor(g).dll = ev.target.value;
    // update only the action bar's enabled state, keep focus in the input
    const btn = $('[data-act="install"]'); const f = formFor(g);
    const model = (S.info && S.info.model) || {};
    const needDll = !model.ready;
    if (btn && needDll) btn.disabled = !f.dll.trim() || busy();
    const sum = $('.actionbar .summary');
    if (sum && needDll) sum.innerHTML = f.dll.trim() ? tv(g.status.installed ? 'sum_re' : 'sum_install', { route: routeName(g.status.installed ? g.status.route : f.route) }) : esc(t('sum_need_model'));
  }
});
document.addEventListener('change', (ev) => {
  if (ev.target.id === 'sel-model') { formFor(selected()).model = ev.target.value; }
  else if (ev.target.id === 'fb-hidden') { const d = S.dialog; d.hidden = ev.target.checked; browse(d, d.res ? d.res.path : ''); }
});
document.addEventListener('keydown', (ev) => {
  if (ev.key === 'Escape' && S.dialog) { closeDialog(); return; }
  if (ev.key === 'Enter' && ev.target.id === 'fb-path') { browse(S.dialog, ev.target.value.trim()); return; }
  if ((ev.ctrlKey && ev.key.toLowerCase() === 'f') || (ev.key === '/' && !/INPUT|SELECT|TEXTAREA/.test(ev.target.tagName))) {
    ev.preventDefault(); const s = $('#search'); if (s) { s.focus(); s.select(); } return;
  }
  if ((ev.key === 'ArrowDown' || ev.key === 'ArrowUp') && !S.dialog && !/INPUT|SELECT|TEXTAREA/.test(ev.target.tagName)) {
    const list = filteredGames(); if (!list.length) return;
    let i = list.findIndex((g) => g.id === S.selId);
    i = Math.max(0, Math.min(list.length - 1, i + (ev.key === 'ArrowDown' ? 1 : -1)));
    S.selId = list[i].id; ev.preventDefault(); renderLibrary(); renderDetail();
    const item = document.querySelector('.item.sel'); if (item) item.scrollIntoView({ block: 'nearest' });
  }
});
// replace broken cover images with the generated tile
document.addEventListener('error', (ev) => {
  const img = ev.target;
  if (img && img.tagName === 'IMG' && img.dataset.fallback !== undefined) {
    const name = img.closest('.item') ? img.closest('.item').title : img.dataset.fallback;
    img.outerHTML = `<div class="tile" style="${tileStyle(name)}">${esc(img.dataset.fallback)}</div>`;
  }
}, true);

/* ------------------------------------------------------------------------------------------- boot */
async function boot() {
  S.themePref = OPT.theme || 'system'; applyTheme();
  try { await loadInfo(); } catch (e) {
    $('#app').innerHTML = `<div class="placeholder"><div><div class="big">${icon('error')}</div><h2>OptiScaler NR</h2><p>${esc(e.message)}</p></div></div>`;
    return;
  }
  renderShell();
  $('#app').removeAttribute('aria-busy');
  connectEvents();
  if (!OPT.noevents) setInterval(() => { api('/api/heartbeat', {}).then(() => { if (!S.connected) { S.connected = true; renderStatus(); } }).catch(() => {}); }, 15000);
  await loadGames(false);
  // last job (if any) - the event stream also sends it, this covers noevents / a slow stream
  try { const r = await api('/api/job'); if (r.job && (!S.job || S.job.id !== r.job.id)) onJob(r.job, false); } catch (e) { /* none */ }
  if (OPT.dialog === 'settings') ACT.settings();
  else if (OPT.dialog === 'confirm' && selected()) ACT.uninstall();
  else if (OPT.dialog === 'browse') openBrowser('model', S.info.home, () => {});
}
boot();
